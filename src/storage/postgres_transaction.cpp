#include "storage/postgres_transaction.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/postgres_catalog.hpp"
#include "storage/postgres_catalog_set.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "postgres_oauth.hpp"
#include "postgres_result.hpp"

namespace duckdb {

PostgresTransaction::PostgresTransaction(PostgresCatalog &postgres_catalog, TransactionManager &manager,
                                         Context &context)
    : Transaction(manager, context), access_mode(postgres_catalog.access_mode),
      isolation_level(postgres_catalog.isolation_level) {
	auto oauth_token_holder = SetThreadLocalOAuthTokenFromSessionOption(context);
	connection = postgres_catalog.GetConnectionPool().Acquire();
}

PostgresTransaction::~PostgresTransaction() = default;

void PostgresTransaction::Start() {
	transaction_state = PostgresTransactionState::TRANSACTION_NOT_YET_STARTED;
}
void PostgresTransaction::Commit(ClientContext &context) {
	if (transaction_state == PostgresTransactionState::TRANSACTION_STARTED) {
		transaction_state = PostgresTransactionState::TRANSACTION_FINISHED;
		GetConnectionRaw().Execute(context, "COMMIT");
	}
	vector<pair<reference<PostgresCatalogSet>, string>> to_promote;
	{
		lock_guard<mutex> l(pending_signatures_lock);
		to_promote = std::move(pending_signatures);
	}
	for (auto &entry : to_promote) {
		entry.first.get().PromoteStalenessSignature(std::move(entry.second));
	}
}
void PostgresTransaction::Rollback(ClientContext &context) {
	if (transaction_state == PostgresTransactionState::TRANSACTION_STARTED) {
		transaction_state = PostgresTransactionState::TRANSACTION_FINISHED;
		GetConnectionRaw().Execute(context, "ROLLBACK");
	}
	lock_guard<mutex> l(pending_signatures_lock);
	pending_signatures.clear();
}

string PostgresTransaction::GetBeginTransactionQuery() {
	return GetBeginTransactionQuery(isolation_level, access_mode);
}

string PostgresTransaction::GetBeginTransactionQuery(PostgresIsolationLevel isolation_level, AccessMode access_mode) {
	string result = "BEGIN TRANSACTION ISOLATION LEVEL ";
	switch (isolation_level) {
	case PostgresIsolationLevel::READ_COMMITTED:
		result += "READ COMMITTED";
		break;
	case PostgresIsolationLevel::REPEATABLE_READ:
		result += "REPEATABLE READ";
		break;
	case PostgresIsolationLevel::SERIALIZABLE:
		result += "SERIALIZABLE";
		break;
	default:
		throw InternalException("Unsupported isolation level");
	}
	if (access_mode == AccessMode::READ_ONLY) {
		result += " READ ONLY";
	}
	return result;
}

PostgresConnection &PostgresTransaction::GetConnectionWithoutTransaction() {
	if (transaction_state == PostgresTransactionState::TRANSACTION_STARTED) {
		throw std::runtime_error("Execution without a Transaction is not possible if a Transaction already started");
	}
	if (access_mode == AccessMode::READ_ONLY) {
		throw std::runtime_error("Execution without a Transaction is not possible in Read Only Mode");
	}
	return connection.GetConnection();
}

PostgresConnection &PostgresTransaction::GetConnection(ClientContext &context) {
	auto &con = GetConnectionRaw();
	if (transaction_state == PostgresTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = PostgresTransactionState::TRANSACTION_STARTED;
		string query = GetBeginTransactionQuery();
		con.Execute(context, query);
	}
	return con;
}

uint64_t PostgresTransaction::GetPoolConnectionId() {
	return connection.Id();
}

PostgresConnection &PostgresTransaction::GetConnectionRaw() {
	return connection.GetConnection();
}

string PostgresTransaction::GetDSN() {
	return GetConnectionRaw().GetDSN();
}

unique_ptr<PostgresResult> PostgresTransaction::Query(ClientContext &context, const string &query) {
	auto &con = GetConnectionRaw();
	if (transaction_state == PostgresTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = PostgresTransactionState::TRANSACTION_STARTED;
		string transaction_start = GetBeginTransactionQuery();
		transaction_start += ";\n";
		return con.Query(context, transaction_start + query);
	}
	return con.Query(context, query);
}

unique_ptr<PostgresResult> PostgresTransaction::QueryWithoutTransaction(ClientContext &context, const string &query) {
	auto &con = GetConnectionRaw();
	if (transaction_state == PostgresTransactionState::TRANSACTION_STARTED) {
		throw std::runtime_error("Execution without a Transaction is not possible if a Transaction already started");
	}
	if (access_mode == AccessMode::READ_ONLY) {
		throw std::runtime_error("Execution without a Transaction is not possible in Read Only Mode");
	}
	return con.Query(context, query);
}

vector<unique_ptr<PostgresResult>> PostgresTransaction::ExecuteQueries(ClientContext &context, const string &queries) {
	auto &con = GetConnectionRaw();
	if (transaction_state == PostgresTransactionState::TRANSACTION_NOT_YET_STARTED) {
		transaction_state = PostgresTransactionState::TRANSACTION_STARTED;
		string transaction_start = GetBeginTransactionQuery();
		transaction_start += ";\n";
		return con.ExecuteQueries(context, transaction_start + queries);
	}
	return con.ExecuteQueries(context, queries);
}

optional_ptr<CatalogEntry> PostgresTransaction::ReferenceEntry(shared_ptr<CatalogEntry> &entry) {
	auto &ref = *entry;
	lock_guard<mutex> l(referenced_entries_lock);
	referenced_entries.emplace(ref, entry);
	return ref;
}

void PostgresTransaction::StageStalenessSignature(PostgresCatalogSet &catalog_set, string signature) {
	lock_guard<mutex> l(pending_signatures_lock);
	pending_signatures.emplace_back(catalog_set, std::move(signature));
}

string PostgresTransaction::GetTemporarySchema(ClientContext &context) {
	if (temporary_schema.empty()) {
		auto result = Query(context, "SELECT nspname FROM pg_namespace WHERE oid = pg_my_temp_schema();");
		if (result->Count() < 1) {
			// no temporary tables exist yet in this connection
			// create a random temporary table and return
			Query(context, "CREATE TEMPORARY TABLE __internal_temporary_table(i INTEGER)");
			result = Query(context, "SELECT nspname FROM pg_namespace WHERE oid = pg_my_temp_schema();");
			if (result->Count() < 1) {
				throw BinderException("Could not find temporary schema pg_temp_NNN for this connection");
			}
		}
		temporary_schema = result->GetString(0, 0);
	}
	return temporary_schema;
}

PostgresTransaction &PostgresTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<PostgresTransaction>();
}

} // namespace duckdb
