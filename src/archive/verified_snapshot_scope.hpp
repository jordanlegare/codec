#pragma once

#include <codec/archive.hpp>

namespace codec::detail {

class VerifiedArchiveSnapshotScope {
 public:
  VerifiedArchiveSnapshotScope(VerifiedArchiveSnapshotScope&& other) noexcept;
  VerifiedArchiveSnapshotScope& operator=(VerifiedArchiveSnapshotScope&&) = delete;
  ~VerifiedArchiveSnapshotScope();

  VerifiedArchiveSnapshotScope(const VerifiedArchiveSnapshotScope&) = delete;
  VerifiedArchiveSnapshotScope& operator=(const VerifiedArchiveSnapshotScope&) =
      delete;

 private:
  friend Result<VerifiedArchiveSnapshotScope> activate_verified_archive_snapshot(
      const CodaArchive& archive, const VerifiedArchiveSnapshot& snapshot);

  VerifiedArchiveSnapshotScope(
      const CodaArchive* previous_archive,
      const VerifiedArchiveSnapshot* previous_snapshot) noexcept
      : previous_archive_(previous_archive),
        previous_snapshot_(previous_snapshot),
        active_(true) {}

  const CodaArchive* previous_archive_{};
  const VerifiedArchiveSnapshot* previous_snapshot_{};
  bool active_{false};
};

Result<VerifiedArchiveSnapshotScope> activate_verified_archive_snapshot(
    const CodaArchive& archive, const VerifiedArchiveSnapshot& snapshot);

}  // namespace codec::detail
