#include "include/pivot_rdf.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

// Minimal stub bind: return 2 VARCHAR columns
struct StubBindData : public TableFunctionData {
	idx_t row_count = 2;
};

struct StubGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static unique_ptr<FunctionData> StubBind(ClientContext &context, TableFunctionBindInput &input,
                                         vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<StubBindData>();
	auto &fs = FileSystem::GetFileSystem(context);

	string pattern = input.inputs[0].GetValue<string>();
	auto glob_results = fs.Glob(pattern);
	if (glob_results.empty()) {
		throw IOException("No files found matching: " + pattern);
	}

	names.push_back("subject");
	return_types.push_back(LogicalType::VARCHAR);
	names.push_back("info");
	return_types.push_back(LogicalType::VARCHAR);

	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> StubGlobalInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<StubGlobalState>();
}

static void StubFunc(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = (StubBindData &)*input.bind_data;
	auto &global_state = (StubGlobalState &)*input.global_state;

	if (global_state.offset >= bind_data.row_count) {
		output.SetCardinality(0);
		return;
	}

	idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.row_count - global_state.offset);
	for (idx_t i = 0; i < count; i++) {
		output.SetValue(0, i, Value("subject_" + to_string(global_state.offset + i)));
		output.SetValue(1, i, Value("info_" + to_string(global_state.offset + i)));
	}
	global_state.offset += count;
	output.SetCardinality(count);
}

void RegisterPivotRDF(ExtensionLoader &loader) {
	TableFunction tf("pivot_rdf", {LogicalType::VARCHAR}, StubFunc, StubBind, StubGlobalInit);
	tf.named_parameters["file_type"] = LogicalType::VARCHAR;
	tf.named_parameters["strict_parsing"] = LogicalType::BOOLEAN;
	tf.named_parameters["prefix_expansion"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(tf);
}

} // namespace duckdb
