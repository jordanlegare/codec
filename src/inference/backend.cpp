#include <codec/inference.hpp>

#include <memory>
#include <string>

namespace codec {
namespace {

class UnavailableBackend final : public SeparationBackend {
 public:
  std::string name() const override { return "unavailable"; }
  bool available() const noexcept override { return false; }
  Result<SeparationResult> separate(
      const SeparationRequest&) override {
    return fail<SeparationResult>(
        ErrorCode::model_incompatible,
        "no compatible neural separation ModelBundle is installed; "
        "original audio remains available without a derived stem");
  }
};

}  // namespace

std::unique_ptr<SeparationBackend> default_separation_backend() {
  return std::make_unique<UnavailableBackend>();
}

}  // namespace codec
