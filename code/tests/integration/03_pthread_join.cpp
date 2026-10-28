// M3 gate: pthread_create/pthread_join equivalent on a bare blob.
//
// The blob hand-writes clone(2)/futex(2): 100 iterations of stack+TLS mmap,
// clone, child runs and exits, parent waits on the tid word until CLEARTID
// wakes it, then unmaps. Asserts guest-visible results, thread lifecycle
// events, memory-event balance and the svc trace (including the first case
// where `seq` and `id` diverge).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/observer/trace/svc_trace.hpp"
#include "support/blob_loader.hpp"
#include "support/test_harness.hpp"
#include "support/trace_path.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint64_t kIterations = 100;
constexpr uint64_t kOutSize = 0x1000;

// Offsets in the guest output page; see 03_pthread_join.S.
constexpr uint64_t kOffIterations = 0x30;
constexpr uint64_t kOffCounter = 0x40;
constexpr uint64_t kOffTpOk = 0x58;
constexpr uint64_t kOffTidOk = 0x60;
constexpr uint64_t kOffAlready = 0x68;
constexpr uint64_t kOffFutexOk = 0x70;
constexpr uint64_t kOffFailures = 0x78;
constexpr uint64_t kOffTidWord = 0x80;

Result<uint64_t> readWord(RuntimeInstance& guest, GuestVA out, uint64_t offset) {
    uint64_t value = 0;
    auto read = guest.readMemory(out + offset, &value, sizeof(value));
    if (!read.ok()) {
        return read.error();
    }
    return value;
}

uint64_t readOr(RuntimeInstance& guest, GuestVA out, uint64_t offset) {
    auto value = readWord(guest, out, offset);
    return value.ok() ? value.value() : ~0ULL;
}

Result<std::vector<observer::SvcEvent>> readTrace(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        return Error{ ErrorCode::NotFound, "cannot read " + path };
    }
    std::vector<observer::SvcEvent> records;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto record = observer::parseSvcTraceLine(line);
        if (!record.has_value()) {
            return Error{ ErrorCode::Internal, "unparseable trace line: " + line };
        }
        records.push_back(std::move(record).value());
    }
    return records;
}

void runJoin() {
    auto blob = test::loadBlob("03_pthread_join.bin");
    CHECK_RESULT(blob);
    if (!blob.ok()) {
        return;
    }

    RuntimeConfig config;
    config.trace_path = test::tracePath("03_pthread_join.svc.log");
    std::remove(config.trace_path.c_str());

    auto instance = RuntimeInstance::create(config);
    CHECK_RESULT(instance);
    if (!instance.ok()) {
        return;
    }
    RuntimeInstance& guest = *instance.value();

    auto code = guest.mapMemory(blob.value().size(), PermRead | PermExec, "join-code");
    CHECK_RESULT(code);
    if (!code.ok()) {
        return;
    }
    CHECK_RESULT(guest.writeMemory(code.value(), blob.value().data(), blob.value().size()));

    auto out = guest.mapMemory(kOutSize, PermReadWrite, "join-out");
    CHECK_RESULT(out);
    if (!out.ok()) {
        return;
    }

    std::atomic<int> created{0};
    std::atomic<int> exited{0};
    std::mutex tid_mutex;
    std::set<uint32_t> created_tids;
    std::set<uint32_t> exited_tids;
    guest.addThreadListener([&](const observer::ThreadEvent& event) {
        if (event.kind == observer::ThreadEventKind::Created) {
            ++created;
            std::lock_guard<std::mutex> lock(tid_mutex);
            created_tids.insert(event.tid);
        } else if (event.kind == observer::ThreadEventKind::Exited) {
            ++exited;
            std::lock_guard<std::mutex> lock(tid_mutex);
            exited_tids.insert(event.tid);
        }
    });

    std::atomic<int> maps{0};
    std::atomic<int> unmaps{0};
    guest.addMemoryListener([&](const observer::MemoryEvent& event) {
        if (event.op == observer::MemoryOp::Map) {
            ++maps;
        } else if (event.op == observer::MemoryOp::Unmap) {
            ++unmaps;
        }
    });

    std::array<uint64_t, 8> args{};
    args[0] = out.value().value();
    auto call = guest.callFunction(code.value(), args);
    CHECK_RESULT(call);
    if (!call.ok()) {
        return;
    }
    CHECK_EQ(call.value().return_value, 0);
    CHECK(!call.value().exit_code.has_value());

    const uint64_t iterations = readOr(guest, out.value(), kOffIterations);
    const uint64_t counter = readOr(guest, out.value(), kOffCounter);
    const uint64_t tp_ok = readOr(guest, out.value(), kOffTpOk);
    const uint64_t tid_ok = readOr(guest, out.value(), kOffTidOk);
    const uint64_t already = readOr(guest, out.value(), kOffAlready);
    const uint64_t futex_ok = readOr(guest, out.value(), kOffFutexOk);
    const uint64_t failures = readOr(guest, out.value(), kOffFailures);
    const uint64_t tid_word = readOr(guest, out.value(), kOffTidWord);

    std::printf("  loops=%llu counter=%llu tp=%llu tid=%llu futex=%llu already=%llu "
                "fail=%llu tid_word=%llu\n",
                static_cast<unsigned long long>(iterations),
                static_cast<unsigned long long>(counter),
                static_cast<unsigned long long>(tp_ok),
                static_cast<unsigned long long>(tid_ok),
                static_cast<unsigned long long>(futex_ok),
                static_cast<unsigned long long>(already),
                static_cast<unsigned long long>(failures),
                static_cast<unsigned long long>(tid_word));

    CHECK_EQ(iterations, kIterations);
    CHECK_EQ(counter, kIterations);
    CHECK_EQ(tp_ok, kIterations);
    CHECK_EQ(tid_ok, kIterations);
    CHECK_EQ(failures, 0);
    CHECK_EQ(already, 0);
    CHECK_EQ(futex_ok, kIterations);
    CHECK_EQ(tid_word, 0);

    // Thread lifecycle: one Created and one Exited per child, no main thread
    // event (its Created fires before the listener exists; it never exits).
    CHECK_EQ(created.load(), kIterations);
    CHECK_EQ(exited.load(), kIterations);
    {
        std::lock_guard<std::mutex> lock(tid_mutex);
        CHECK(created_tids == exited_tids);
        CHECK(created_tids.count(1) == 0);
        CHECK(created_tids.size() == kIterations);
    }

    // Guest mmaps/unmaps balance: two of each per iteration.
    CHECK_EQ(maps.load(), static_cast<int>(kIterations * 2));
    CHECK_EQ(unmaps.load(), static_cast<int>(kIterations * 2));

    auto trace = readTrace(config.trace_path);
    CHECK_RESULT(trace);
    if (!trace.ok()) {
        return;
    }
    const auto& records = trace.value();
    CHECK_EQ(records.size(), kIterations * 8);

    std::printf("  svc.log: %zu record(s); first iteration:\n", records.size());
    for (size_t i = 0; i < std::min<size_t>(8, records.size()); ++i) {
        std::printf("    %s\n", observer::formatSvcTraceLine(records[i]).c_str());
    }

    std::set<uint64_t> clone_results;
    std::set<uint32_t> exit_tids;
    bool saw_divergence = false;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& record = records[i];
        CHECK_EQ(record.id, i);
        if (i > 0 && record.seq < records[i - 1].seq) {
            saw_divergence = true;
        }
        if (record.name == "clone") {
            CHECK_EQ(record.tid, 1);
            CHECK(record.result >= 2);
            clone_results.insert(record.result);
        } else if (record.name == "exit") {
            CHECK(record.tid >= 2);
            CHECK_EQ(record.result, 0);
            exit_tids.insert(record.tid);
        } else if (record.name == "gettid") {
            CHECK_EQ(record.result, record.tid);
        } else if (record.name == "futex") {
            CHECK_EQ(record.tid, 1);
            CHECK_EQ(record.result, 0);
        }
    }

    std::set<uint32_t> clone_tids;
    for (uint64_t tid : clone_results) {
        clone_tids.insert(static_cast<uint32_t>(tid));
    }
    CHECK_EQ(clone_results.size(), kIterations);
    CHECK(clone_tids == exit_tids);
    CHECK(saw_divergence);

    size_t clones = 0, futexes = 0, exits = 0, gettids = 0;
    for (const auto& record : records) {
        clones += record.name == "clone" ? 1 : 0;
        futexes += record.name == "futex" ? 1 : 0;
        exits += record.name == "exit" ? 1 : 0;
        gettids += record.name == "gettid" ? 1 : 0;
    }
    CHECK_EQ(clones, kIterations);
    CHECK_EQ(futexes, kIterations);
    CHECK_EQ(exits, kIterations);
    CHECK_EQ(gettids, kIterations);

    // seq is the call order: every value in [0, records.size()) exactly once.
    std::vector<bool> seen(records.size(), false);
    bool seq_ok = true;
    for (const auto& record : records) {
        if (record.seq >= seen.size() || seen[record.seq]) {
            seq_ok = false;
            break;
        }
        seen[record.seq] = true;
    }
    CHECK(seq_ok);

    std::printf("  thread events: created=%d exited=%d\n", created.load(), exited.load());
}

} // namespace

int main() {
    runJoin();
    return test::finish("03_pthread_join");
}
