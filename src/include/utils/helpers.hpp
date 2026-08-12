#pragma once

#include <duckdb.hpp>
#ifndef DUCKDB_CPP_EXTENSION_ENTRY
#include <duckdb/main/extension_util.hpp>
#endif
#include <type_traits>

// DuckDB's `main` branch (post-1.5) replaced std::string with a dedicated,
// case-insensitive `duckdb::Identifier` type in catalog/parser/appender APIs.
// Its runtime-string constructor is explicit, so std::string values must be
// wrapped at the call site. Older releases don't have the type at all, so we
// feature-detect the header rather than rely on the (dev-suffixed) version.
#if defined(__has_include) && __has_include("duckdb/common/identifier.hpp")
#include "duckdb/common/identifier.hpp"
#define DUCKDB_HAS_IDENTIFIER 1
#endif

// TODO we cannot run these checks because they are not defined for DuckDB
// < 1.4.x #ifndef DUCKDB_MAJOR_VERSION #error "DUCKDB_MAJOR_VERSION is not
// defined"
// ...
#define DUCKDB_VERSION_AT_MOST(major, minor, patch)                            \
  (DUCKDB_MAJOR_VERSION < (major) ||                                           \
   (DUCKDB_MAJOR_VERSION == (major) && DUCKDB_MINOR_VERSION < (minor)) ||      \
   (DUCKDB_MAJOR_VERSION == (major) && DUCKDB_MINOR_VERSION == (minor) &&      \
    DUCKDB_PATCH_VERSION <= (patch)))

#define DUCKDB_VERSION_AT_LEAST(major, minor, patch)                           \
  (DUCKDB_MAJOR_VERSION > (major) ||                                           \
   (DUCKDB_MAJOR_VERSION == (major) && DUCKDB_MINOR_VERSION > (minor)) ||      \
   (DUCKDB_MAJOR_VERSION == (major) && DUCKDB_MINOR_VERSION == (minor) &&      \
    DUCKDB_PATCH_VERSION >= (patch)))

namespace duckdb {

// Wrap a runtime std::string for catalog/parser/appender APIs that take an
// `Identifier` on newer DuckDB. On older versions those APIs take std::string,
// so the value passes through unchanged.
#ifdef DUCKDB_HAS_IDENTIFIER
inline Identifier AsCatalogIdentifier(const std::string &name) {
  return Identifier(name);
}
inline const Identifier &AsCatalogIdentifier(const Identifier &name) {
  return name;
}
#else
inline const std::string &AsCatalogIdentifier(const std::string &name) {
  return name;
}
#endif

// Recover the raw string of a catalog name, whichever type it is carried in.
inline const std::string &AsRawString(const std::string &name) { return name; }
#ifdef DUCKDB_HAS_IDENTIFIER
inline const std::string &AsRawString(const Identifier &name) {
  return name.GetIdentifierName();
}
#endif

// DuckDB's `main` branch made BaseQueryResult's `names` and `types` private,
// reaching them through accessors instead, and switched the names to
// `Identifier`.
#if DUCKDB_VERSION_AT_LEAST(1, 6, 0)
inline const auto &ResultNames(const BaseQueryResult &result) {
  return result.GetNames();
}
inline const vector<LogicalType> &ResultTypes(const BaseQueryResult &result) {
  return result.GetTypes();
}
#else
inline const vector<std::string> &ResultNames(const BaseQueryResult &result) {
  return result.names;
}
inline const vector<LogicalType> &ResultTypes(const BaseQueryResult &result) {
  return result.types;
}
#endif

// Emit a single-row, single-column result into a table function's output.
// DuckDB's `main` branch derives a chunk's cardinality from the sizes of its
// child vectors, which are grown with Vector::Append. There, the old
// write-at-index + SetCardinality pair leaves the child vector empty while
// claiming a cardinality of one, which yields a malformed chunk.
inline void AppendSingleValue(DataChunk &output, Value value) {
#if DUCKDB_VERSION_AT_LEAST(1, 6, 0)
  output.data[0].Append(value);
#else
  output.SetCardinality(1);
  output.SetValue(0, 0, value);
#endif
}

typedef std::string (*simple_tf_t)(ClientContext &);

struct RunOnceTableFunctionState : GlobalTableFunctionState {
  RunOnceTableFunctionState() : run(false){};
  std::atomic<bool> run;

  static unique_ptr<GlobalTableFunctionState> Init(ClientContext &,
                                                   TableFunctionInitInput &) {
    return make_uniq<RunOnceTableFunctionState>();
  }
};

namespace internal {

// The element type of the out_names parameter of a table function's bind
// callback: std::string on DuckDB 1.5 and earlier, Identifier on `main`.
// Deduced from the callback type so both are supported.
template <typename T> struct BindNameType;
template <typename Result, typename Context, typename Input, typename Types,
          typename Names>
struct BindNameType<Result (*)(Context, Input, Types, Names)> {
  using type = typename std::remove_reference<Names>::type::value_type;
};
using bind_name_t = typename BindNameType<table_function_bind_t>::type;

unique_ptr<FunctionData> SingleBoolResultBind(ClientContext &,
                                              TableFunctionBindInput &,
                                              vector<LogicalType> &out_types,
                                              vector<bind_name_t> &out_names);

unique_ptr<FunctionData> SingleStringResultBind(ClientContext &,
                                                TableFunctionBindInput &,
                                                vector<LogicalType> &,
                                                vector<bind_name_t> &);

bool ShouldRun(TableFunctionInput &input);

template <typename Func> struct CallFunctionHelper;

template <> struct CallFunctionHelper<std::string (*)(ClientContext &)> {
  static std::string call(ClientContext &context, TableFunctionInput &input,
                          std::string (*f)(ClientContext &)) {
    return f(context);
  }
};

template <>
struct CallFunctionHelper<std::string (*)(ClientContext &,
                                          TableFunctionInput &)> {
  static std::string call(ClientContext &context, TableFunctionInput &input,
                          std::string (*f)(ClientContext &,
                                           TableFunctionInput &)) {
    return f(context, input);
  }
};

template <typename Func, Func func>
void TableFunc(ClientContext &context, TableFunctionInput &input,
               DataChunk &output) {
  if (!ShouldRun(input)) {
    return;
  }

  const std::string result =
      CallFunctionHelper<Func>::call(context, input, func);
  AppendSingleValue(output, Value(result));
}

#ifdef DUCKDB_CPP_EXTENSION_ENTRY
template <typename Func, Func func>
void RegisterTF(ExtensionLoader &loader, const char *name) {
  TableFunction tf(name, {}, internal::TableFunc<Func, func>,
                   internal::SingleStringResultBind,
                   RunOnceTableFunctionState::Init);
  loader.RegisterFunction(tf);
}
#else
template <typename Func, Func func>
void RegisterTF(DatabaseInstance &instance, const char *name) {
  TableFunction tf(name, {}, internal::TableFunc<Func, func>,
                   internal::SingleStringResultBind,
                   RunOnceTableFunctionState::Init);
  ExtensionUtil::RegisterFunction(instance, tf);
}
#endif

} // namespace internal

#ifdef DUCKDB_CPP_EXTENSION_ENTRY
#define REGISTER_TF(name, func)                                                \
  internal::RegisterTF<decltype(&func), &func>(loader, name)
#else
#define REGISTER_TF(name, func)                                                \
  internal::RegisterTF<decltype(&func), &func>(instance, name)
#endif

} // namespace duckdb
