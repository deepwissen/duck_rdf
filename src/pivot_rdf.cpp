#include "include/pivot_rdf.hpp"
#include "include/rdf_profiler.hpp"
#include "include/I_triples_buffer.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace duckdb {

// Normalise a profile type-name to a DuckDB LogicalType.
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

// ============================================================
// Bind data — stores profiled schema
// ============================================================

struct PivotRDFBindData : public TableFunctionData {
	vector<string> file_paths;
	// Column names and types derived from profiling
	vector<string> col_names;
	vector<LogicalType> col_types;
};

struct PivotRDFGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

// ============================================================
// Bind — profiles the file and builds schema, but scan is stub
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

	ITriplesBuffer::FileType file_type = ITriplesBuffer::UNKNOWN;
	auto ft_it = input.named_parameters.find("file_type");
	if (ft_it != input.named_parameters.end())
		file_type = ITriplesBuffer::ParseFileTypeString(ft_it->second.GetValue<string>());

	bool strict_parsing = true;
	auto sp_it = input.named_parameters.find("strict_parsing");
	if (sp_it != input.named_parameters.end())
		strict_parsing = sp_it->second.GetValue<bool>();

	// --- Profile all files to discover predicates and types ---
	RDFProfileAccumulator accumulator;
	for (auto &file_path : result->file_paths) {
		ITriplesBuffer::FileType ft = file_type;
		if (ft == ITriplesBuffer::UNKNOWN)
			ft = ITriplesBuffer::DetectFileTypeFromPath(file_path);
		try {
			switch (ft) {
			case ITriplesBuffer::TURTLE:
			case ITriplesBuffer::NTRIPLES:
			case ITriplesBuffer::NQUADS:
			case ITriplesBuffer::TRIG:
				ProfileFileSerd(file_path, fs, ft, strict_parsing, accumulator);
				break;
			case ITriplesBuffer::XML:
				ProfileFileXML(file_path, fs, strict_parsing, accumulator);
				break;
			default:
				throw IOException("Cannot determine file type for: " + file_path);
			}
		} catch (const std::runtime_error &re) {
			throw IOException(re.what());
		}
	}

	// Build sorted predicate list
	const auto &profiles = accumulator.GetProfiles();
	std::vector<std::string> pred_uris;
	pred_uris.reserve(profiles.size());
	for (const auto &kv : profiles)
		pred_uris.push_back(kv.first);
	std::sort(pred_uris.begin(), pred_uris.end());

	// Schema: graph, subject, then one VARCHAR col per predicate (simplified)
	names.push_back("graph");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("subject");
	return_types.push_back(LogicalType::VARCHAR);
	for (const auto &pred : pred_uris) {
		names.push_back(pred);
		// For this bisect: all columns as VARCHAR regardless of profile
		return_types.push_back(LogicalType::VARCHAR);
	}

	return std::move(result);
}

// ============================================================
// Stub scan — returns 0 rows (just tests that bind succeeds)
// ============================================================

static unique_ptr<GlobalTableFunctionState> PivotRDFGlobalInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<PivotRDFGlobalState>();
}

static void PivotRDFFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	output.SetCardinality(0);
}

// ============================================================
// Registration
// ============================================================

void RegisterPivotRDF(ExtensionLoader &loader) {
	TableFunction tf("pivot_rdf", {LogicalType::VARCHAR}, PivotRDFFunc, PivotRDFBind, PivotRDFGlobalInit);
	tf.named_parameters["file_type"] = LogicalType::VARCHAR;
	tf.named_parameters["strict_parsing"] = LogicalType::BOOLEAN;
	tf.named_parameters["prefix_expansion"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(tf);
}

} // namespace duckdb
