#include "util/status.h"

namespace ltm {

const char* StatusCodeName(StatusCode c) {
  switch (c) {
    case StatusCode::kOk: return "kOk";
    case StatusCode::kNotFound: return "kNotFound";
    case StatusCode::kInvalidArgument: return "kInvalidArgument";
    case StatusCode::kOutOfSpace: return "kOutOfSpace";
    case StatusCode::kPageFull: return "kPageFull";
    case StatusCode::kUnderflow: return "kUnderflow";
    case StatusCode::kChecksumMismatch: return "kChecksumMismatch";
    case StatusCode::kIoError: return "kIoError";
    case StatusCode::kCorruption: return "kCorruption";
    case StatusCode::kNotSupported: return "kNotSupported";
    case StatusCode::kDuplicateKey: return "kDuplicateKey";
    case StatusCode::kDimensionMismatch: return "kDimensionMismatch";
    case StatusCode::kNotImplemented: return "kNotImplemented";
  }
  return "kUnknown";
}

std::string Status::ToString() const {
  if (ok()) {
    return "kOk";
  }
  if (message_.empty()) {
    return StatusCodeName(code_);
  }
  return std::string(StatusCodeName(code_)) + ": " + message_;
}

}  // namespace ltm
