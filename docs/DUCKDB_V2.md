# Running the UI on DuckDB 2.0.0

DuckDB 2.0.0 made three changes that the hosted frontend cannot absorb. Each one
breaks the UI in a different way, and none of them is visible as a build error —
the extension compiles clean against `main` and then the app fails at runtime.

This document records what changed, how each was diagnosed, and what the fix is.

## The frontend is a fixed target

The app is a bundle served from `https://ui.duckdb.org`. It is not open source
and it is not versioned against the DuckDB you are running: the same bundle is
served regardless of the `duckdb-ui/<version>-<extver>(<platform>)` User-Agent
the extension sends.

That bundle is built from the TypeScript client in
[`ts/pkgs/duckdb-ui-client`](../ts/pkgs/duckdb-ui-client), which is in this
repository. Extracting every serialization field id the deployed bundle reads
gives exactly the set the source declares — `100`–`106`, `200`–`201`. So when
the bundle cannot read something, it is not a stale deployment. There is nothing
newer to deploy.

MotherDuck's staging build (`app.staging.motherduck.com`) is a different commit
with an identical field-id set, so it is not ahead either.

**Consequence:** the wire format between this extension and the app is *our*
protocol, not the server's storage format. When DuckDB changes what it puts on
the wire, the extension has to keep sending what the app understands. Every fix
below follows that principle, and each is verified the same way — byte-compare
the raw HTTP response against stock 1.5.5, rather than checking whether the page
looked right.

## 1. VARCHAR vectors — `Expected field id 102 but got 107`

`Vector::Serialize` gates on `ShouldSerialize(StorageVersion::V2_0_0)` and
writes three fields — `107 byte_data_length`, `108 length_data`, `109 byte_data`
— where it previously wrote a `102` list of strings.

The app's `readVector` requires `102`. This kills the *first* query the app runs,
so the UI never initializes at all.

**Fix.** Pin the serializer's storage compatibility for query results:

```cpp
SerializationOptions ResultSerializationOptions() {
  SerializationOptions options;
  options.storage_compatibility =
      StorageCompatibility::FromIndex(StorageVersion::V1_5_5);
  return options;
}
```

passed to the one `BinarySerializer::Serialize` call that emits results. Unpin
when the client learns the `107/108/109` form.

## 2. NULL literals — `unrecognized type id: 1`

DuckDB 1.5.x resolved a bare `NULL` literal to `INTEGER`. 2.0.0 leaves the column
typed `SQLNULL`, which is type id `1` — and the app's copy of `LogicalTypeId`
starts at `BOOLEAN: 10` and has no entry for it.

```
select NULL as x
  1.5.5   … 650001 64000d ffff …      0x0d = 13 = INTEGER
  v2      … 650001 640001 ffff …      0x01 =  1 = SQLNULL
```

The app generates these columns itself — it emits `NULL as <name>` for columns
it has no values for — so this fires on startup and again after every cell run.

A `SQLNULL` vector is physically `INT32` and serializes byte-for-byte like an
all-null `INTEGER`: comparing raw responses, the entire payload matched except
the two type-id bytes, at every nesting depth.

**Fix.** Rewrite `SQLNULL` to `INTEGER` in the declared types, recursing through
`LIST`, `ARRAY`, `MAP`, `STRUCT` and `UNION`. Subtrees containing no `SQLNULL`
are returned unchanged, so aliases and type modifiers survive.

Use `UnionType::GetMemberCount` for unions, not `StructType::GetChildCount` —
the latter also returns the hidden tag member and rebuilding from it corrupts
the type.

## 3. A phantom token — cells run twice

Symptom: running `create table foo (bar varchar);` creates the table *and*
reports `Catalog Error: Table with name "foo" already exists!`.

The extension executes one statement per request, so the app was sending two.
Confirmed by logging (below):

```
/ddb/run  conn=connection_X6yD889FNZmR  sql=create table zebra (bar varchar)
/ddb/run  conn=connection_X6yD889FNZmR  sql=create table zebra (bar varchar);
```

Same connection, one Run.

The cause is `/ddb/tokenize`. The app splits a cell into statements using these
offsets, closing a statement at each `;` token. DuckDB 2.0.0 appends one extra
token starting at `content.size()` — one past the last character, pointing at
nothing — which 1.5.x did not:

| input | len | 1.5.5 offsets | 2.0.0 offsets |
| --- | --- | --- | --- |
| `select 1` | 8 | `[0,7]` | `[0,7,`**`8`**`]` |
| `select 1;` | 9 | `[0,7,8]` | `[0,7,8,`**`9`**`]` |
| `create table zebra (bar varchar);` | 33 | 8 tokens, last at 32 | 9 tokens, last at **33** |

The phantom reopens a statement after the semicolon already ended one, and the
cell is run a second time.

**Fix.** Drop tokens that do not point at a character. This makes `/ddb/tokenize`
byte-identical to 1.5.5 for every input tried but one: a comment-only cell, where
2.0.0 emits a `COMMENT` token the enum did not previously have. That is a real
token at a real offset, so it is left alone.

## Debugging the app

The app cannot be stepped through, and reading its minified source produces
plausible wrong answers. Set `DUCKDB_UI_LOG_REQUESTS=1` to record what it
actually sends:

```
$ DUCKDB_UI_LOG_REQUESTS=1 duckdb -unsigned mydata.duckdb \
    -cmd "LOAD ui; CALL start_ui_server();"
ui: /ddb/run conn=connection_X6yD889FNZmR desc=- sql=create table zebra (bar varchar)
```

For the third bug, that log plus a real browser driven over the Chrome DevTools
Protocol — same driver, fresh profile and database each run — showed the double
execution was deterministic on 2.0.0 and never happened on 1.5.5. That is what
turned "the app is doing something weird" into a specific tokenizer diff.

## Verifying a change

Run the same query against stock 1.5.5 and against 2.0.0 with the build under
test, and compare the response bytes:

```
curl -s -X POST -H "Origin: http://localhost:$PORT" -H "Content-Type: text/plain" \
     --data "select NULL as x" "http://localhost:$PORT/ddb/run" | xxd
```

Anything other than "identical" is a difference the app may not survive. The
current build is byte-identical to 1.5.5 across the type space — strings, empty
strings, non-ASCII, wide strings, integers, hugeint, decimal, double, boolean,
date, timestamp, timestamptz, interval, blob, list, struct, map, array, enum,
bignum — and across bare and nested `NULL`s in list, struct, map, union,
list-of-list and struct-of-list, over single and multiple rows.

## A note on `Vector::Serialize`

`serialization.cpp` calls `serialized_vector.Serialize(object, row_count)`. In
2.0.0 that second parameter is `bool compressed_serialization`, not a count — the
count now comes from `Vector::size()`. Any non-zero `row_count` converts to
`true`, which matches the default, so this is currently harmless. It is still a
trap and should be cleaned up separately.
