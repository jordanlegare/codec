# Stage F.5 Bounded Orchestration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add deterministic, resource-bounded multi-partition scheduling that composes F.1 partitions through F.4 location resolution, F.3 exact retrieval, and F.2 worker execution with one explicit outcome per input partition.

**Architecture:** Add one synchronous sequential scheduler in `src/distributed/scheduler.cpp` and an additive public API in `<codec/distributed.hpp>`. The scheduler preflights the full batch before side effects, assigns workers by stable input-position round robin, selects each record's first canonical F.4 candidate, captures F.4/F.3/F.2 failures per partition, and continues without retry or concurrency.

**Tech Stack:** C++20, existing CODEC `Result`/`Error`, `DistributedPartition`, `DistributedLocationIndex`, `ObjectStoreBackend`, `DistributedWorker`, CMake/CTest, GitHub Actions GCC/Clang/sanitizer matrix.

**Spec:** `docs/superpowers/specs/2026-08-29-stage-f5-bounded-orchestration-design.md`

## Global Constraints

- Preserve F.1 `DistributedPartition` and CDP1 bytes exactly.
- Preserve F.2, F.3, and F.4 public behavior; F.5 composes rather than bypasses those APIs.
- Preserve CODA, S0/S1/D, provenance, Stage E, C ABI, and CLI semantics.
- Use deterministic input-order scheduling and worker assignment `partition_index % workers.size()`.
- Use F.4 candidate index `0` as the only F.5 location-selection rule.
- Preflight the complete batch and aggregate scheduler-owned bounds before backend reads or worker calls.
- Treat F.4/F.3/F.2 failures after preflight as partition-local outcomes and continue later partitions.
- Perform no retries even when a nested error is retryable.
- Add no threads/concurrency, RPC/network workers, worker discovery/health/attestation, backend registry/routing/failover, leases/heartbeats/exactly-once semantics, persistent/global location catalogs, deployment integration, or performance/scale claim.
- Current version remains `0.2.0`; the API is additive.

---

### Task 1: Add failing F.5 scheduler contract tests

**Files:**
- Create: `tests/test_distributed_scheduler.cpp`
- Modify: `CMakeLists.txt` test source list only

**Interfaces:**
- Consumes: existing F.1 `partition_exact_records`, F.4 `build_distributed_location_index`, F.3/F.2 mockable abstract interfaces.
- Produces: compile-time expectations for `DistributedPartitionOutcomeStatus`, `DistributedSchedulingLimits`, `DistributedPartitionOutcome`, `DistributedScheduleResult`, and `schedule_partitions(...)`.

- [ ] **Step 1: Add reusable test doubles and exact-record helpers**

Create deterministic helpers in `tests/test_distributed_scheduler.cpp` following the repository's `test.hpp` style:

```cpp
namespace {

codec::ExtractedRecord make_record(codec::StreamId stream,
                                   std::uint64_t sequence,
                                   std::initializer_list<unsigned char> bytes);

class MapBackend final : public codec::ObjectStoreBackend {
 public:
  std::string name() const override { return "scheduler-backend"; }
  codec::Result<std::vector<std::byte>> read_range(
      const codec::ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) override;

  std::map<std::string, std::vector<std::byte>> objects;
  std::size_t calls{};
  std::optional<std::string> fail_key;
};

class RecordingWorker final : public codec::DistributedWorker {
 public:
  RecordingWorker(std::string worker_name, std::string processor_name)
      : worker_name_(std::move(worker_name)),
        processor_name_(std::move(processor_name)) {}

  std::string name() const override { return worker_name_; }
  std::string processor_name() const override { return processor_name_; }
  codec::Result<std::vector<codec::ProcessorOutput>> execute(
      std::span<const codec::ExtractedRecord> inputs) override;

  std::size_t calls{};
  std::vector<codec::Sha256> seen_hashes;
  bool fail{};

 private:
  std::string worker_name_;
  std::string processor_name_;
};

codec::DistributedRecordLocation make_location(
    const codec::ExtractedRecord& record,
    std::string key,
    std::string store = "test");

}  // namespace
```

The backend must verify the requested offset/length against its stored bytes, increment `calls`, return a configured error for `fail_key`, and otherwise return the exact requested bytes. The worker must increment `calls`, record exact input hashes, optionally return `codec::fail<std::vector<codec::ProcessorOutput>>(codec::ErrorCode::internal, "worker failure")`, and otherwise emit one valid deterministic `ProcessorOutput` derived from the first input using existing processor-output invariants.

- [ ] **Step 2: Add the successful multi-partition composition test**

Build at least four exact records and partition them with `maximum_records_per_partition = 1`, so at least four F.1 partitions exist. Register two placement candidates per record in reverse registration order, build F.4, create two workers, and call:

```cpp
std::array<codec::DistributedWorker*, 2> workers{&worker_a, &worker_b};
auto scheduled = codec::schedule_partitions(
    workers, backend, *index, *partitions,
    codec::DistributedSchedulingLimits{});
```

Assert:

```cpp
EXPECT_TRUE(scheduled);
EXPECT_EQ(scheduled->partitions.size(), partitions->size());
EXPECT_EQ(scheduled->succeeded, partitions->size());
for (std::size_t i = 0; i < scheduled->partitions.size(); ++i) {
  const auto& outcome = scheduled->partitions[i];
  EXPECT_EQ(outcome.partition_index, i);
  EXPECT_EQ(outcome.worker_index, i % 2);
  EXPECT_EQ(outcome.status,
            codec::DistributedPartitionOutcomeStatus::succeeded);
  EXPECT_TRUE(outcome.execution.has_value());
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_EQ(outcome.partition_identity, (*partitions)[i].identity);
  EXPECT_EQ(outcome.selected_locations.size(),
            (*partitions)[i].records.size());
  EXPECT_EQ(outcome.selected_locations.front().object.key,
            "a-" + std::to_string(i));
}
```

Also assert worker calls are 2/2, backend calls equal record count, and F.2 execution results carry the expected worker names.

- [ ] **Step 3: Add partition-local failure/continuation tests**

Create three partitions and prove each failure stage separately:

1. Omit F.4 locations for partition 0: status `location_unavailable`; backend and assigned worker receive no call for it.
2. Configure backend failure for partition 1 selected key: status `retrieval_failed`; worker receives no call for it.
3. Configure partition 1's assigned worker to fail in a separate scenario: status `execution_failed`.

In each scenario assert the later partition still executes and its `worker_index` is computed from original input position rather than previous successes.

For all failure outcomes assert `error.has_value()` with the nested error code except `location_unavailable`, which must use a scheduler-created non-retryable `ErrorCode::network` error message explaining that no complete indexed placement exists. No retries occur.

- [ ] **Step 4: Add schedule-level preflight/failure tests**

Before any backend/worker call, assert `schedule_partitions` fails for:

```text
- non-empty partitions with zero workers
- any null worker pointer
- maximum_partitions == 0
- maximum_workers == 0
- maximum_total_records == 0
- maximum_total_payload_bytes == 0
- partitions.size() > maximum_partitions
- workers.size() > maximum_workers
- aggregate record count > maximum_total_records
- aggregate payload bytes > maximum_total_payload_bytes
- tampered partition CDP1 identity
- a partition link whose stream differs from partition.stream
- duplicate partition identity in the same schedule batch
```

Assert backend and all workers still have zero calls after each preflight rejection.

- [ ] **Step 5: Add empty-batch behavior**

Call with an empty partition span and an empty worker span. Assert success with `succeeded == 0` and an empty outcome vector, with no backend call.

- [ ] **Step 6: Register the test source in CMake and commit RED**

Add `tests/test_distributed_scheduler.cpp` to the unit test source list adjacent to the other distributed tests.

Commit message:

```text
test: define Stage F.5 bounded scheduler contract
```

Expected CI result: compilation must fail because F.5 public scheduler types/functions do not yet exist. The failure must reach the new test translation unit; unrelated configuration/build failures do not count as RED evidence.

---

### Task 2: Implement the minimal deterministic scheduler

**Files:**
- Modify: `include/codec/distributed.hpp`
- Create: `src/distributed/scheduler.cpp`
- Modify: `CMakeLists.txt` library source list

**Interfaces:**
- Consumes:
  - `resolve_partition_location_candidates(const DistributedLocationIndex&, const DistributedPartition&, DistributedLocationQueryLimits)`
  - `retrieve_partition_records(ObjectStoreBackend&, const DistributedPartition&, std::span<const DistributedRecordLocation>, DistributedRetrievalLimits)`
  - `execute_partition(DistributedWorker&, const DistributedPartition&, std::span<const ExtractedRecord>, DistributedExecutionLimits)`
- Produces:

```cpp
enum class DistributedPartitionOutcomeStatus {
  succeeded,
  location_unavailable,
  retrieval_failed,
  execution_failed,
};

struct DistributedSchedulingLimits {
  std::size_t maximum_partitions{4096};
  std::size_t maximum_workers{256};
  std::size_t maximum_total_records{16384};
  std::uint64_t maximum_total_payload_bytes{256ULL * 1024ULL * 1024ULL};
  DistributedLocationQueryLimits location_query{};
  DistributedRetrievalLimits retrieval{};
  DistributedExecutionLimits execution{};
};

struct DistributedPartitionOutcome {
  Sha256 partition_identity{};
  StreamId stream{};
  std::size_t partition_index{};
  std::size_t worker_index{};
  DistributedPartitionOutcomeStatus status{
      DistributedPartitionOutcomeStatus::location_unavailable};
  std::vector<DistributedRecordLocation> selected_locations;
  std::optional<DistributedExecutionResult> execution;
  std::optional<Error> error;
};

struct DistributedScheduleResult {
  std::size_t succeeded{};
  std::vector<DistributedPartitionOutcome> partitions;
};

Result<DistributedScheduleResult> schedule_partitions(
    std::span<DistributedWorker* const> workers,
    ObjectStoreBackend& backend,
    const DistributedLocationIndex& index,
    std::span<const DistributedPartition> partitions,
    DistributedSchedulingLimits limits = {});
```

- [ ] **Step 1: Add the additive public API**

Add `#include <optional>` to `include/codec/distributed.hpp` and append the exact API above after F.4 declarations. Do not change existing F.1–F.4 signatures or defaults.

- [ ] **Step 2: Add schedule-level limit and batch preflight helpers**

In `src/distributed/scheduler.cpp`, include `<codec/distributed.hpp>` and `internal.hpp`, plus `<cstddef>`, `<cstdint>`, `<new>`, `<set>`, `<utility>`, and `<vector>`.

Implement:

```cpp
Result<void> validate_scheduling_limits(const DistributedSchedulingLimits& limits) {
  if (limits.maximum_partitions == 0 || limits.maximum_workers == 0 ||
      limits.maximum_total_records == 0 ||
      limits.maximum_total_payload_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed scheduling limits must be non-zero");
  }
  return {};
}
```

Then preflight the whole batch before side effects:

```cpp
if (partitions.empty()) {
  return DistributedScheduleResult{};
}
if (workers.empty()) {
  return fail<DistributedScheduleResult>(
      ErrorCode::invalid_argument,
      "distributed scheduling requires at least one worker");
}
if (partitions.size() > limits.maximum_partitions ||
    workers.size() > limits.maximum_workers) {
  return fail<DistributedScheduleResult>(ErrorCode::resource_exhausted, ...);
}
for (auto* worker : workers) {
  if (worker == nullptr) return fail<DistributedScheduleResult>(...);
}
```

Use `std::set<Sha256>` for duplicate identities. For every partition, require non-empty membership, all links use `partition.stream`, recompute `detail::distributed_partition_identity(partition.stream, partition.records)`, reject duplicates, and accumulate record/payload totals with subtraction-before-add overflow-safe checks against scheduler limits. Reject malformed/tampered input before any F.4/F.3/F.2 call.

- [ ] **Step 3: Implement deterministic per-partition orchestration**

Reserve exactly `partitions.size()` outcomes, then loop by index:

```cpp
for (std::size_t partition_index = 0;
     partition_index < partitions.size(); ++partition_index) {
  const auto& partition = partitions[partition_index];
  const auto worker_index = partition_index % workers.size();
  auto* worker = workers[worker_index];

  DistributedPartitionOutcome outcome{
      .partition_identity = partition.identity,
      .stream = partition.stream,
      .partition_index = partition_index,
      .worker_index = worker_index,
  };

  auto resolved = resolve_partition_location_candidates(
      index, partition, limits.location_query);
```

If resolution itself returns an error, record `location_unavailable` and copy/move the nested error into `outcome.error`; continue.

If `resolved->complete == false`, set:

```cpp
outcome.status = DistributedPartitionOutcomeStatus::location_unavailable;
outcome.error = Error{
    ErrorCode::network,
    "distributed scheduling has no complete indexed placement for the partition",
    false,
};
```

and continue without F.3/F.2.

For a complete result, require one non-empty candidate set per partition member; this should follow F.4's contract, but fail the schedule internally if the impossible invariant is violated. Select `candidates.front()` for each record, preserving record order.

Call F.3:

```cpp
auto retrieved = retrieve_partition_records(
    backend, partition, outcome.selected_locations, limits.retrieval);
if (!retrieved) {
  outcome.status = DistributedPartitionOutcomeStatus::retrieval_failed;
  outcome.error = retrieved.error();
  result.partitions.push_back(std::move(outcome));
  continue;
}
```

Then F.2:

```cpp
auto executed = execute_partition(
    *worker, partition, retrieved->records, limits.execution);
if (!executed) {
  outcome.status = DistributedPartitionOutcomeStatus::execution_failed;
  outcome.error = executed.error();
  result.partitions.push_back(std::move(outcome));
  continue;
}

outcome.status = DistributedPartitionOutcomeStatus::succeeded;
outcome.execution = std::move(*executed);
++result.succeeded;
result.partitions.push_back(std::move(outcome));
```

Do not retry or skip later partitions after a partition-local failure.

Wrap the implementation in `try/catch (const std::bad_alloc&)` and return `ErrorCode::resource_exhausted` on scheduler allocation failure.

- [ ] **Step 4: Add the scheduler source to the library and run exact-head CI**

Add `src/distributed/scheduler.cpp` next to other `src/distributed/*` sources.

Expected result: the RED scheduler tests compile and pass; existing tests remain green.

Commit message:

```text
feat: add Stage F.5 bounded partition orchestration
```

---

### Task 3: Prove installed-package composition and update truthful status docs

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `AI_WORKSHEET.md`
- Modify: `CHANGELOG.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: implemented F.5 installed public API.
- Produces: external package proof and repository status claims constrained to the tested synchronous deterministic scope.

- [ ] **Step 1: Extend the installed-package consumer**

After the existing F.1→F.4→F.3→F.2 composition, construct at least two one-record partitions, one location index, the package consumer's backend, and two local workers. Call `schedule_partitions` through installed `<codec/distributed.hpp>` and require:

```cpp
if (!scheduled || scheduled->succeeded != partitions->size() ||
    scheduled->partitions.size() != partitions->size() ||
    scheduled->partitions[0].worker_index != 0 ||
    scheduled->partitions[1].worker_index != 1 ||
    scheduled->partitions[0].status !=
        codec::DistributedPartitionOutcomeStatus::succeeded ||
    scheduled->partitions[1].status !=
        codec::DistributedPartitionOutcomeStatus::succeeded) {
  return <new unique nonzero code>;
}
```

Keep the package consumer deterministic and filesystem/network independent.

- [ ] **Step 2: Update `AI_WORKSHEET.md` active work record**

Set the active record to Stage F.5 with:

```yaml
task: Add bounded deterministic multi-partition scheduling that composes F.1 partitions through F.4 location resolution, F.3 exact retrieval, and F.2 worker execution with per-partition outcomes.
base_ref: origin/main
base_head_sha: 1e48a16b11378897b0311f4c198c443d1a1bb976
work_branch: automation/stage-f5-bounded-orchestration
current_version: 0.2.0
active_roadmap_stage: F — F.1 partitioning, F.2 one-partition execution, F.3 exact retrieval, and F.4 bounded location indexing are merged; bounded multi-partition scheduling/orchestration is the next unmet dependency.
continuity_evidence:
  - git_head: main at 1e48a16b11378897b0311f4c198c443d1a1bb976
  - open_prs: stale unrelated draft PR 26 preserved; F.5 uses its own branch/PR
  - exact_head_ci: F.4 final head passed GCC, Clang, sanitizers, tests, and install before merge
  - roadmap_issue: issue 10 records F.4 complete and requires a fresh Stage F dependency selection
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: distributed
touched_truth_classes: []
current_behavior_verified_from: [code, tests, cli, changelog]
new_capability_claim: A caller can schedule a bounded ordered batch of exact F.1 partitions across an ordered worker pool, deterministically select canonical F.4 placements, materialize through F.3, execute through F.2, and receive one explicit outcome per partition.
change_class: generic_stream_abstraction
```

BEFORE/AFTER and proof text must match the design spec exactly in scope.

- [ ] **Step 3: Update CHANGELOG and README**

Add one Unreleased bullet stating that F.5 is implemented as deterministic synchronous multi-partition orchestration with batch preflight, input-position round robin, canonical-first placement selection, and per-partition outcomes.

Update README implemented Stage F bullets/roadmap text to include F.5, and narrow the remaining planned list to concurrency/network worker execution, persistent/global discovery/catalogs, provider routing/failover, operational benchmarks, and deployment integrations.

Explicitly say no concurrency/thread pool, RPC/network execution, retries/failover/exactly-once, persistent/global catalog, deployment, or scale claim is added.

- [ ] **Step 4: Commit docs/package proof**

Commit message:

```text
docs: record Stage F.5 orchestration capability
```

---

### Task 4: Exact-head verification, review, merge, and roadmap completion

**Files:**
- No product-code changes unless verification finds a concrete defect.
- Update GitHub issue `#10` after merge.

**Interfaces:**
- Consumes: final branch exact head.
- Produces: merge evidence and next dependency audit.

- [ ] **Step 1: Verify exact-head GitHub Actions**

Required exact-head jobs must pass:

```text
build (gcc)
build (clang)
sanitizers
```

The jobs must include configure/build, full CTest suite, install/package consumer, AI contract, and the repository's normal sanitizer settings. Do not use a prior commit's CI result.

- [ ] **Step 2: Inspect failing logs if any**

For any failed job, read the exact failing step/log, identify root cause, add/adjust the smallest test if needed, fix the implementation, push a new commit, and restart exact-head verification. Never merge on stale green CI after the head moves.

- [ ] **Step 3: Perform final diff/claim review**

Check:

```text
- only F.5 scheduler/test/docs/package files changed
- no generated artifacts or credentials
- no existing F.1–F.4 API mutation
- no S0/S1/D/provenance/CODA semantic change
- no hidden retry/concurrency/network/discovery/failover behavior
- public API outcome/error ownership is safe and deterministic
- full-batch malformed input is rejected before side effects
- partition-local failures continue later partitions
- README/CHANGELOG claims match tests
```

- [ ] **Step 4: Merge only the exact green PR head**

Use expected-head protection and squash merge. Verify post-merge `main` points to the returned merge commit and the PR is merged.

- [ ] **Step 5: Record Stage F.5 completion in issue #10**

Add a completion comment containing:

```yaml
result:
  outcome: bounded deterministic synchronous multi-partition scheduling/orchestration implemented
  branch: automation/stage-f5-bounded-orchestration
  base_head_sha: 1e48a16b11378897b0311f4c198c443d1a1bb976
  tdd_red_head_sha: <exact RED sha>
  final_exact_ci_head_sha: <exact GREEN sha>
  pull_request: <PR URL>
  ci_run: <run URL/id>
  merge_commit: <merge SHA>
  capability_delta: F.1 partitions now compose through F.4 canonical placement selection, F.3 exact retrieval, and F.2 execution across an ordered worker pool with deterministic per-partition outcomes
  remaining_planned: concurrent/network worker execution, persistent/global location discovery/catalogs, provider routing/failover, retries/leases/exactly-once semantics, operational benchmarks, deployment integrations
```

Then perform a fresh merged-tree Stage F dependency audit. Do not pre-name F.6 unless the merged implementation/dependency graph makes the next gate clear.
