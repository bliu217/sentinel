#include "storage/sqlite_store.h"

#include <sqlite3.h>
#include <windows.h>

#include <algorithm>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace sentinel::storage {
namespace {

using Clock = std::chrono::system_clock;

class SqliteError : public std::runtime_error {
public:
    SqliteError(sqlite3* db, const std::string& operation)
        : std::runtime_error(operation + ": " + sqlite3_errmsg(db)), code_(sqlite3_errcode(db)) {}
    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw SqliteError(db, "prepare statement");
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    void bind(std::int32_t index, std::int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) throw SqliteError(db_, "bind integer");
    }
    void bind(std::int32_t index, double value) {
        if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK) throw SqliteError(db_, "bind number");
    }
    void bind(std::int32_t index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            throw SqliteError(db_, "bind text");
        }
    }
    void stepDone() {
        if (sqlite3_step(statement_) != SQLITE_DONE) throw SqliteError(db_, "execute statement");
    }
    [[nodiscard]] bool stepRow() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw SqliteError(db_, "read statement");
    }
    void reset() {
        if (sqlite3_reset(statement_) != SQLITE_OK) throw SqliteError(db_, "reset statement");
        sqlite3_clear_bindings(statement_);
    }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_{};
};

void execute(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        sqlite3_free(message);
        throw SqliteError(db, "execute SQL");
    }
}

[[nodiscard]] std::int64_t millis(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

[[nodiscard]] Clock::time_point fromMillis(std::int64_t value) {
    return Clock::time_point{std::chrono::milliseconds(value)};
}

[[nodiscard]] std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length == 0) throw std::runtime_error("Invalid process name encoding");
    std::string output(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), length, nullptr, nullptr) == 0) {
        throw std::runtime_error("Invalid process name encoding");
    }
    return output;
}

[[nodiscard]] std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length == 0) throw std::runtime_error("Invalid process name in database");
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), length) == 0) {
        throw std::runtime_error("Invalid process name in database");
    }
    return output;
}

[[nodiscard]] std::int64_t scalar(sqlite3* db, const char* sql) {
    Statement query(db, sql);
    if (!query.stepRow()) throw std::runtime_error("Missing SQLite scalar result");
    return sqlite3_column_int64(query.get(), 0);
}

[[nodiscard]] std::string newStoreId() {
    std::random_device source;
    constexpr char digits[] = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int index = 0; index < 32; ++index) id += digits[source() & 15];
    return id;
}

}  // namespace

SQLiteStore::SQLiteStore(const std::filesystem::path& path, std::uint64_t maxBytes) : maxBytes_(maxBytes) {
    if (maxBytes != 0 && maxBytes < 64 * 1024) {
        throw std::invalid_argument("SQLite size cap must be at least 64 KiB");
    }
    std::filesystem::create_directories(path.parent_path());
    if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
        const std::string detail = db_ != nullptr ? sqlite3_errmsg(db_) : "open failed";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Open SQLite database: " + detail);
    }
    try {
        execute(db_, "PRAGMA busy_timeout=5000; PRAGMA foreign_keys=ON; PRAGMA auto_vacuum=INCREMENTAL;");
        execute(db_, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        execute(db_, "CREATE TABLE IF NOT EXISTS ticks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, utc_ms INTEGER NOT NULL, system_utc_ms INTEGER,"
            "cpu_percent REAL, memory_used_bytes INTEGER, memory_available_bytes INTEGER,"
            "process_utc_ms INTEGER);");
        execute(db_, "CREATE INDEX IF NOT EXISTS ticks_utc ON ticks(utc_ms,id);");
        execute(db_, "CREATE TABLE IF NOT EXISTS process_samples ("
            "tick_id INTEGER NOT NULL REFERENCES ticks(id) ON DELETE CASCADE,"
            "pid INTEGER NOT NULL, creation_time INTEGER NOT NULL, image_name TEXT NOT NULL,"
            "cpu_percent REAL, working_set_bytes INTEGER NOT NULL, private_bytes INTEGER NOT NULL);");
        execute(db_, "CREATE INDEX IF NOT EXISTS processes_tick ON process_samples(tick_id);");
        execute(db_, "CREATE TABLE IF NOT EXISTS anomalies ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, tick_id INTEGER NOT NULL REFERENCES ticks(id) ON DELETE CASCADE,"
            "event_type INTEGER NOT NULL, state INTEGER NOT NULL, value REAL NOT NULL,"
            "occurred_utc_ms INTEGER NOT NULL, observed_utc_ms INTEGER NOT NULL);");
        execute(db_, "CREATE INDEX IF NOT EXISTS anomalies_tick ON anomalies(tick_id);");
        execute(db_, "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);");
        {
            Statement readId(db_, "SELECT value FROM metadata WHERE key='store_id'");
            if (readId.stepRow()) {
                storeId_ = reinterpret_cast<const char*>(sqlite3_column_text(readId.get(), 0));
            } else {
                storeId_ = newStoreId();
                Statement insertId(db_, "INSERT INTO metadata(key,value) VALUES('store_id',?1)");
                insertId.bind(1, storeId_);
                insertId.stepDone();
            }
        }
        if (maxBytes_ != 0) {
            const std::uint64_t pages = maxBytes_ / static_cast<std::uint64_t>(scalar(db_, "PRAGMA page_size;"));
            const std::string cap = "PRAGMA max_page_count=" + std::to_string(pages) + ";";
            execute(db_, cap.c_str());
            if (static_cast<std::uint64_t>(scalar(db_, "PRAGMA max_page_count;")) < pages) {
                throw std::runtime_error("Unable to configure SQLite size cap");
            }
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

SQLiteStore::~SQLiteStore() { sqlite3_close(db_); }

void SQLiteStore::writeTick(
    Clock::time_point tickUtc,
    const std::optional<telemetry::SystemSample>& system,
    const std::optional<telemetry::ProcessSnapshot>& processes,
    const std::vector<attribution::AnomalyEvent>& anomalies) {
    std::uint64_t estimatedBytes = 512 + 128 * anomalies.size();
    if (processes) {
        for (const telemetry::ProcessSample& process : processes->processes) {
            estimatedBytes += 128 + process.imageName.size() * 4;
        }
    }
    if (maxBytes_ != 0 && estimatedBytes > maxBytes_ / 2) {
        throw std::runtime_error("A single tick is too large for the SQLite size cap");
    }
    for (;;) {
        execute(db_, "BEGIN IMMEDIATE;");
        try {
            writeTickOnce(tickUtc, system, processes, anomalies);
            execute(db_, "COMMIT;");
            break;
        } catch (const SqliteError& error) {
            if (sqlite3_get_autocommit(db_) == 0) {
                try { execute(db_, "ROLLBACK;"); } catch (...) {}
            }
            const std::int64_t count = countTicks();
            if (error.code() != SQLITE_FULL || count <= 1 ||
                !deleteOldest(std::min<std::int64_t>(256, count - 1))) throw;
        } catch (...) {
            if (sqlite3_get_autocommit(db_) == 0) {
                try { execute(db_, "ROLLBACK;"); } catch (...) {}
            }
            throw;
        }
    }
    enforceCap();
}

void SQLiteStore::writeTickOnce(
    Clock::time_point tickUtc,
    const std::optional<telemetry::SystemSample>& system,
    const std::optional<telemetry::ProcessSnapshot>& processes,
    const std::vector<attribution::AnomalyEvent>& anomalies) {
        Statement tick(db_, "INSERT INTO ticks(utc_ms,system_utc_ms,cpu_percent,memory_used_bytes,"
            "memory_available_bytes,process_utc_ms) VALUES(?1,?2,?3,?4,?5,?6)");
        tick.bind(1, millis(tickUtc));
        if (system) {
            tick.bind(2, millis(system->utcTimestamp));
            tick.bind(3, system->cpuUsagePercent);
            tick.bind(4, static_cast<std::int64_t>(system->memoryUsedBytes));
            tick.bind(5, static_cast<std::int64_t>(system->memoryAvailableBytes));
        }
        if (processes) tick.bind(6, millis(processes->time.utc));
        tick.stepDone();
        const std::int64_t tickId = sqlite3_last_insert_rowid(db_);
        Statement process(db_, "INSERT INTO process_samples VALUES(?1,?2,?3,?4,?5,?6,?7)");
        if (processes) {
            for (const telemetry::ProcessSample& item : processes->processes) {
                process.bind(1, tickId);
                process.bind(2, static_cast<std::int64_t>(item.identity.processId));
                process.bind(3, static_cast<std::int64_t>(item.identity.creationTime));
                process.bind(4, utf8(item.imageName));
                if (item.cpuUsagePercent) process.bind(5, *item.cpuUsagePercent);
                process.bind(6, static_cast<std::int64_t>(item.workingSetBytes));
                process.bind(7, static_cast<std::int64_t>(item.privateBytes));
                process.stepDone();
                process.reset();
            }
        }
        Statement event(db_, "INSERT INTO anomalies(tick_id,event_type,state,value,occurred_utc_ms,"
            "observed_utc_ms) VALUES(?1,?2,?3,?4,?5,?6)");
        for (const attribution::AnomalyEvent& item : anomalies) {
            const auto elapsed = item.observedAt.steady - item.detection.timestamp;
            const auto occurred = item.detection.occurredAtUtc.value_or(
                item.observedAt.utc - std::chrono::duration_cast<Clock::duration>(elapsed));
            event.bind(1, tickId);
            event.bind(2, static_cast<std::int64_t>(item.detection.type));
            event.bind(3, static_cast<std::int64_t>(item.detection.state));
            event.bind(4, item.detection.value);
            event.bind(5, millis(occurred));
            event.bind(6, millis(item.observedAt.utc));
            event.stepDone();
            event.reset();
        }
}

void SQLiteStore::pruneBefore(Clock::time_point cutoff) {
    Statement remove(db_, "DELETE FROM ticks WHERE utc_ms < ?1");
    remove.bind(1, millis(cutoff));
    remove.stepDone();
    execute(db_, "PRAGMA incremental_vacuum;");
}

bool SQLiteStore::deleteOldest(std::int64_t count) {
    if (countTicks() == 0) return false;
    Statement remove(db_, "DELETE FROM ticks WHERE id IN (SELECT id FROM ticks ORDER BY utc_ms,id LIMIT ?1)");
    remove.bind(1, count);
    remove.stepDone();
    return true;
}

std::uint64_t SQLiteStore::allocatedBytes() const {
    const std::int64_t pages = scalar(db_, "PRAGMA page_count;");
    const std::int64_t pageSize = scalar(db_, "PRAGMA page_size;");
    return static_cast<std::uint64_t>(pages * pageSize);
}

void SQLiteStore::enforceCap() {
    while (maxBytes_ != 0 && allocatedBytes() > maxBytes_) {
        const std::int64_t count = countTicks();
        if (count <= 1 || !deleteOldest(std::min<std::int64_t>(256, count - 1))) {
            throw std::runtime_error("SQLite size cap cannot hold the latest tick");
        }
        execute(db_, "PRAGMA incremental_vacuum;");
    }
    if (maxBytes_ != 0) {
        const std::uint64_t pages = maxBytes_ /
            static_cast<std::uint64_t>(scalar(db_, "PRAGMA page_size;"));
        const std::string cap = "PRAGMA max_page_count=" + std::to_string(pages) + ";";
        execute(db_, cap.c_str());
    }
}

std::int64_t SQLiteStore::countTicks() const { return scalar(db_, "SELECT count(*) FROM ticks;"); }

std::vector<StoredTick> SQLiteStore::readTicks(
    Clock::time_point from, Clock::time_point to,
    std::int64_t afterUtcMilliseconds, std::int64_t afterId, std::int64_t limit) const {
    std::vector<StoredTick> output;
    Statement ticks(db_, "SELECT id,utc_ms,system_utc_ms,cpu_percent,memory_used_bytes,"
        "memory_available_bytes,process_utc_ms FROM ticks WHERE utc_ms>=?1 AND utc_ms<?2 "
        "AND (utc_ms>?3 OR (utc_ms=?3 AND id>?4)) ORDER BY utc_ms,id LIMIT ?5");
    ticks.bind(1, millis(from));
    ticks.bind(2, millis(to));
    ticks.bind(3, afterUtcMilliseconds);
    ticks.bind(4, afterId);
    ticks.bind(5, limit);
    Statement processes(db_, "SELECT pid,creation_time,image_name,cpu_percent,working_set_bytes,private_bytes "
        "FROM process_samples WHERE tick_id=?1 ORDER BY pid,creation_time");
    Statement anomalies(db_, "SELECT id,event_type,state,value,occurred_utc_ms,observed_utc_ms "
        "FROM anomalies WHERE tick_id=?1 ORDER BY id");
    while (ticks.stepRow()) {
        StoredTick tick;
        tick.id = sqlite3_column_int64(ticks.get(), 0);
        tick.utcMilliseconds = sqlite3_column_int64(ticks.get(), 1);
        if (sqlite3_column_type(ticks.get(), 2) != SQLITE_NULL) {
            tick.system = telemetry::SystemSample{
                .cpuUsagePercent = sqlite3_column_double(ticks.get(), 3),
                .memoryUsedBytes = static_cast<std::uint64_t>(sqlite3_column_int64(ticks.get(), 4)),
                .memoryAvailableBytes = static_cast<std::uint64_t>(sqlite3_column_int64(ticks.get(), 5)),
                .utcTimestamp = fromMillis(sqlite3_column_int64(ticks.get(), 2))};
        }
        if (sqlite3_column_type(ticks.get(), 6) != SQLITE_NULL) {
            tick.processes = telemetry::ProcessSnapshot{};
            tick.processes->time.utc = fromMillis(sqlite3_column_int64(ticks.get(), 6));
            processes.bind(1, tick.id);
            while (processes.stepRow()) {
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(processes.get(), 2));
                telemetry::ProcessSample item;
                item.identity.processId = static_cast<std::uint32_t>(sqlite3_column_int64(processes.get(), 0));
                item.identity.creationTime = static_cast<std::uint64_t>(sqlite3_column_int64(processes.get(), 1));
                item.imageName = wide(name != nullptr ? name : "");
                if (sqlite3_column_type(processes.get(), 3) != SQLITE_NULL) {
                    item.cpuUsagePercent = sqlite3_column_double(processes.get(), 3);
                }
                item.workingSetBytes = static_cast<std::uint64_t>(sqlite3_column_int64(processes.get(), 4));
                item.privateBytes = static_cast<std::uint64_t>(sqlite3_column_int64(processes.get(), 5));
                tick.processes->processes.push_back(std::move(item));
            }
            processes.reset();
        }
        anomalies.bind(1, tick.id);
        while (anomalies.stepRow()) {
            tick.anomalies.push_back(StoredAnomaly{
                .id = sqlite3_column_int64(anomalies.get(), 0),
                .type = static_cast<detection::EventType>(sqlite3_column_int(anomalies.get(), 1)),
                .state = static_cast<detection::EventState>(sqlite3_column_int(anomalies.get(), 2)),
                .value = sqlite3_column_double(anomalies.get(), 3),
                .occurredUtcMilliseconds = sqlite3_column_int64(anomalies.get(), 4),
                .observedUtcMilliseconds = sqlite3_column_int64(anomalies.get(), 5)});
        }
        anomalies.reset();
        output.push_back(std::move(tick));
    }
    return output;
}

void RetentionManager::pruneOnStartup(Clock::time_point now) {
    store_.pruneBefore(now - std::chrono::hours(24 * 30));
    store_.enforceCap();
}

}  // namespace sentinel::storage
