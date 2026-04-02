#include "include/pivot_rdf.hpp"
#include "include/rdf_profiler.hpp"
#include "include/I_triples_buffer.hpp"
#include "include/serd_buffer.hpp"
#include "include/xml_buffer.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {

// ============================================================
// Type mapping (simplified: no MAP, LIST, UNION)
// ============================================================

static LogicalType TypeNameToLogical(const std::string &name) {
	if (name == "IRI" || name == "BLANK" || name == "VARCHAR")
		return LogicalType::VARCHAR;
	if (name == "BOOLEAN")
		return LogicalType::BOOLEAN;
	if (name == "TINYINT")
		return LogicalType::TINYINT;
	if (name == "SMALLINT")
		return LogicalType::SMALLINT;
	if (name == "INTEGER")
		return LogicalType::INTEGER;
	if (name == "BIGINT")
		return LogicalType::BIGINT;
	if (name == "HUGEINT")
		return LogicalType::HUGEINT;
	if (name == "FLOAT")
		return LogicalType::FLOAT;
	if (name == "DOUBLE")
		return LogicalType::DOUBLE;
	if (name == "DECIMAL")
		return LogicalType::DOUBLE;
	if (name == "DATE")
		return LogicalType::DATE;
	if (name == "TIMESTAMP")
		return LogicalType::TIMESTAMP;
	return LogicalType::VARCHAR;
}

// Derive a single scalar LogicalType for a predicate from its profile.
// No MAP, LIST, or UNION — just pick the single most common type, or VARCHAR if mixed.
static LogicalType DeriveScalarType(const PredicateProfile &profile) {
	std::vector<LogicalType> distinct_types;
	for (const auto &kv : profile.type_stats) {
		LogicalType lt = TypeNameToLogical(kv.first);
		bool already = false;
		for (const auto &dt : distinct_types) {
			if (dt == lt) { already = true; break; }
		}
		if (!already)
			distinct_types.push_back(lt);
	}
	if (distinct_types.size() == 1)
		return distinct_types[0];
	// Mixed types -> VARCHAR
	return LogicalType::VARCHAR;
}

// Convert string to typed Value
static Value StringToTypedValue(const std::string &str, const LogicalType &target) {
	if (target == LogicalType::VARCHAR)
		return Value(str);
	if (target == LogicalType::BOOLEAN)
		return Value::BOOLEAN(str == "true" || str == "1");
	try {
		if (target == LogicalType::TINYINT)
			return Value::TINYINT(static_cast<int8_t>(std::stoi(str)));
		if (target == LogicalType::SMALLINT)
			return Value::SMALLINT(static_cast<int16_t>(std::stoi(str)));
		if (target == LogicalType::INTEGER)
			return Value::INTEGER(static_cast<int32_t>(std::stoi(str)));
		if (target == LogicalType::BIGINT)
			return Value::BIGINT(static_cast<int64_t>(std::stoll(str)));
		if (target == LogicalType::HUGEINT)
			return Value::HUGEINT(static_cast<int64_t>(std::stoll(str)));
		if (target == LogicalType::FLOAT)
			return Value::FLOAT(std::stof(str));
		if (target == LogicalType::DOUBLE)
			return Value::DOUBLE(std::stod(str));
	} catch (...) {
		return Value(str);
	}
	return Value(str);
}

// ============================================================
// State structs
// ============================================================

struct PivotColInfo {
	std::string predicate;
	LogicalType col_type;
};

struct PivotRDFBindData : public TableFunctionData {
	vector<string> file_paths;
	ITriplesBuffer::FileType file_type = ITriplesBuffer::UNKNOWN;
	bool strict_parsing = true;
	bool expand_prefixes = false;
	std::vector<PivotColInfo> columns;
	std::unordered_map<std::string, idx_t> pred_to_col;
};

struct PivotRDFGlobalState : public GlobalTableFunctionState {
	std::mutex lock;
	idx_t next_file = 0;
	idx_t file_count = 0;
	idx_t MaxThreads() const override { return file_count; }
};

struct PivotRDFLocalState : public LocalTableFunctionState {
	std::unique_ptr<ITriplesBuffer> ib;
	DataChunk raw_chunk;
	idx_t raw_chunk_pos = 0;

	std::string current_graph;
	std::string current_subject;
	bool has_pending = false;

	// Per-column: first value seen as string + whether set
	std::vector<std::string> col_values;
	std::vector<bool> col_has_value;
};

// ============================================================
// Open file
// ============================================================

static unique_ptr<ITriplesBuffer> PivotOpenFile(const string &file_path, ITriplesBuffer::FileType ft, FileSystem &fs,
                                                bool strict_parsing, bool expand_prefixes) {
	if (ft == ITriplesBuffer::UNKNOWN)
		ft = ITriplesBuffer::DetectFileTypeFromPath(file_path);
	switch (ft) {
	case ITriplesBuffer::TURTLE:
	case ITriplesBuffer::NQUADS:
	case ITriplesBuffer::NTRIPLES:
	case ITriplesBuffer::TRIG:
		return make_uniq<SerdBuffer>(file_path, "", &fs, strict_parsing, expand_prefixes, ft);
	case ITriplesBuffer::XML:
		return make_uniq<XMLBuffer>(file_path, "", &fs, strict_parsing, expand_prefixes, ft);
	default:
		throw IOException("Cannot determine file type for: " + file_path);
	}
}

// ============================================================
// Bind
// ============================================================

static unique_ptr<FunctionData> PivotRDFBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<PivotRDFBindData>();
	auto &fs = FileSystem::GetFileSystem(context);

	string pattern = input.inputs[0].GetValue<string>();
	auto glob_results = fs.Glob(pattern);
	if (glob_results.empty())
		throw IOException("No files found matching: " + pattern);
	for (auto &info : glob_results)
		result->file_paths.push_back(std::move(info.path));

	auto ft_it = input.named_parameters.find("file_type");
	if (ft_it != input.named_parameters.end())
		result->file_type = ITriplesBuffer::ParseFileTypeString(ft_it->second.GetValue<string>());

	auto sp_it = input.named_parameters.find("strict_parsing");
	if (sp_it != input.named_parameters.end())
		result->strict_parsing = sp_it->second.GetValue<bool>();

	// Profile
	RDFProfileAccumulator accumulator;
	for (auto &file_path : result->file_paths) {
		ITriplesBuffer::FileType ft = result->file_type;
		if (ft == ITriplesBuffer::UNKNOWN)
			ft = ITriplesBuffer::DetectFileTypeFromPath(file_path);
		try {
			switch (ft) {
			case ITriplesBuffer::TURTLE:
			case ITriplesBuffer::NTRIPLES:
			case ITriplesBuffer::NQUADS:
			case ITriplesBuffer::TRIG:
				ProfileFileSerd(file_path, fs, ft, result->strict_parsing, accumulator);
				break;
			case ITriplesBuffer::XML:
				ProfileFileXML(file_path, fs, result->strict_parsing, accumulator);
				break;
			default:
				throw IOException("Cannot determine file type for: " + file_path);
			}
		} catch (const std::runtime_error &re) {
			throw IOException(re.what());
		}
	}

	// Build columns with real types (but no MAP/LIST/UNION)
	const auto &profiles = accumulator.GetProfiles();
	std::vector<std::string> pred_uris;
	for (const auto &kv : profiles)
		pred_uris.push_back(kv.first);
	std::sort(pred_uris.begin(), pred_uris.end());

	for (const auto &pred : pred_uris) {
		PivotColInfo col;
		col.predicate = pred;
		col.col_type = DeriveScalarType(profiles.at(pred));
		result->columns.push_back(col);
	}
	for (idx_t i = 0; i < result->columns.size(); i++)
		result->pred_to_col[result->columns[i].predicate] = i;

	// Schema
	names.push_back("graph");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("subject");
	return_types.push_back(LogicalType::VARCHAR);
	for (const auto &col : result->columns) {
		names.push_back(col.predicate);
		return_types.push_back(col.col_type);
	}

	return std::move(result);
}

// ============================================================
// Init
// ============================================================

static unique_ptr<GlobalTableFunctionState> PivotRDFGlobalInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = (PivotRDFBindData &)*input.bind_data;
	auto state = make_uniq<PivotRDFGlobalState>();
	state->file_count = bind_data.file_paths.size();
	return state;
}

static unique_ptr<LocalTableFunctionState> PivotRDFLocalInit(ExecutionContext &context,
                                                             TableFunctionInitInput &input,
                                                             GlobalTableFunctionState *) {
	auto &bind_data = (PivotRDFBindData &)*input.bind_data;
	auto state = make_uniq<PivotRDFLocalState>();
	idx_t ncols = bind_data.columns.size();
	state->col_values.resize(ncols);
	state->col_has_value.resize(ncols, false);

	vector<LogicalType> raw_types(6, LogicalType::VARCHAR);
	state->raw_chunk.Initialize(Allocator::Get(context.client), raw_types);

	return state;
}

// ============================================================
// Helpers
// ============================================================

static void EmitRow(PivotRDFLocalState &state, const PivotRDFBindData &bind_data,
                    DataChunk &output, idx_t out_idx) {
	output.SetValue(0, out_idx, Value(state.current_graph));
	output.SetValue(1, out_idx, Value(state.current_subject));
	for (idx_t i = 0; i < bind_data.columns.size(); i++) {
		if (state.col_has_value[i]) {
			output.SetValue(2 + i, out_idx,
			                StringToTypedValue(state.col_values[i], bind_data.columns[i].col_type));
		} else {
			output.SetValue(2 + i, out_idx, Value(bind_data.columns[i].col_type)); // typed NULL
		}
	}
}

static void ResetAccum(PivotRDFLocalState &state) {
	for (idx_t i = 0; i < state.col_values.size(); i++) {
		state.col_values[i].clear();
		state.col_has_value[i] = false;
	}
}

// ============================================================
// Scan
// ============================================================

static void PivotRDFFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = (PivotRDFLocalState &)*input.local_state;
	auto &global_state = (PivotRDFGlobalState &)*input.global_state;
	auto &bind_data = (PivotRDFBindData &)*input.bind_data;
	auto &fs = FileSystem::GetFileSystem(context);

	idx_t out_idx = 0;

	while (out_idx < STANDARD_VECTOR_SIZE) {
		if (state.raw_chunk_pos >= state.raw_chunk.size()) {
			if (state.ib) {
				state.raw_chunk.Reset();
				state.ib->PopulateChunk(state.raw_chunk);
				state.raw_chunk_pos = 0;

				if (state.raw_chunk.size() == 0) {
					if (state.has_pending) {
						EmitRow(state, bind_data, output, out_idx++);
						ResetAccum(state);
						state.has_pending = false;
					}
					state.ib.reset();
				} else {
					continue;
				}
			}

			idx_t file_idx;
			{
				std::lock_guard<std::mutex> lk(global_state.lock);
				if (global_state.next_file >= global_state.file_count)
					break;
				file_idx = global_state.next_file++;
			}

			const string &file_path = bind_data.file_paths[file_idx];
			try {
				auto new_ib = PivotOpenFile(file_path, bind_data.file_type, fs,
				                            bind_data.strict_parsing, bind_data.expand_prefixes);
				new_ib->StartParse();
				vector<column_t> all_cols = {0, 1, 2, 3, 4, 5};
				new_ib->SetColumnIds(all_cols);
				state.ib = std::move(new_ib);
			} catch (const std::runtime_error &re) {
				throw IOException(re.what());
			}

			state.raw_chunk.Reset();
			state.ib->PopulateChunk(state.raw_chunk);
			state.raw_chunk_pos = 0;

			if (state.raw_chunk.size() == 0) {
				state.ib.reset();
				continue;
			}
		}

		idx_t row = state.raw_chunk_pos++;

		auto ReadStr = [&](idx_t col) -> std::string {
			auto &vec = state.raw_chunk.data[col];
			auto *data = FlatVector::GetData<string_t>(vec);
			auto &validity = FlatVector::Validity(vec);
			if (!validity.RowIsValid(row))
				return std::string();
			return data[row].GetString();
		};

		std::string graph = ReadStr(0);
		std::string subject = ReadStr(1);
		std::string predicate = ReadStr(2);
		std::string object = ReadStr(3);

		if (state.has_pending && (graph != state.current_graph || subject != state.current_subject)) {
			EmitRow(state, bind_data, output, out_idx++);
			ResetAccum(state);
			state.current_graph = graph;
			state.current_subject = subject;
		} else if (!state.has_pending) {
			state.current_graph = graph;
			state.current_subject = subject;
			state.has_pending = true;
		}

		auto it = bind_data.pred_to_col.find(predicate);
		if (it != bind_data.pred_to_col.end()) {
			idx_t col_idx = it->second;
			if (!state.col_has_value[col_idx]) {
				state.col_values[col_idx] = object;
				state.col_has_value[col_idx] = true;
			}
		}
	}

	output.SetCardinality(out_idx);
}

// ============================================================
// Registration
// ============================================================

void RegisterPivotRDF(ExtensionLoader &loader) {
	TableFunction tf("pivot_rdf", {LogicalType::VARCHAR}, PivotRDFFunc, PivotRDFBind, PivotRDFGlobalInit,
	                 PivotRDFLocalInit);
	tf.named_parameters["file_type"] = LogicalType::VARCHAR;
	tf.named_parameters["strict_parsing"] = LogicalType::BOOLEAN;
	tf.named_parameters["prefix_expansion"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(tf);
}

} // namespace duckdb
