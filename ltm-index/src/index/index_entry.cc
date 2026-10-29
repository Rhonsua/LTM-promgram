#include "index/index_entry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ltm {

// ---------------------------------------------------------------------------
// Point
// ---------------------------------------------------------------------------
Status Point::Validate() const {
  for (double v : coords_) {
    if (std::isnan(v)) {
      return InvalidArgument("point coordinate is NaN");
    }
  }
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// Mbr
// ---------------------------------------------------------------------------
Mbr Mbr::Invalid(uint16_t dim) {
  Mbr m;
  m.lo.resize(dim);
  m.hi.resize(dim);
  for (uint16_t d = 0; d < dim; ++d) {
    m.lo[d] = std::numeric_limits<double>::infinity();
    m.hi[d] = -std::numeric_limits<double>::infinity();
  }
  return m;
}

bool Mbr::valid() const {
  const uint16_t d = lo.dim();
  if (d == 0 || d != hi.dim()) {
    return false;
  }
  for (uint16_t i = 0; i < d; ++i) {
    if (lo[i] > hi[i]) {
      return false;
    }
  }
  return true;
}

Status Mbr::Validate() const {
  const uint16_t d = lo.dim();
  if (d == 0) {
    return InvalidArgument("MBR dimension is 0 (1-D records should use an empty MBR with dim=0)");
  }
  if (d != hi.dim()) {
    return Status::Error(StatusCode::kDimensionMismatch,
                         "MBR lo/hi dimension mismatch: " + std::to_string(d) + " vs " +
                             std::to_string(hi.dim()));
  }
  for (uint16_t i = 0; i < d; ++i) {
    if (std::isnan(lo[i]) || std::isnan(hi[i])) {
      return InvalidArgument("MBR contains a NaN coordinate");
    }
    if (lo[i] > hi[i]) {
      return InvalidArgument("MBR dim " + std::to_string(i) + " lo > hi: " +
                             std::to_string(lo[i]) + " > " + std::to_string(hi[i]));
    }
  }
  return Status::Ok();
}

namespace {

// 面积累乘：遇到非法（lo > hi）或退化维度时返回 0，避免 inf * 0 = NaN 造成
// 面积比较全部失效（会让 R 树选叶策略静默退化为"永远选第一个候选"）。
double SafeArea(const Mbr& m) {
  const uint16_t d = m.lo.dim();
  if (d == 0 || d != m.hi.dim()) {
    return 0.0;
  }
  double a = 1.0;
  for (uint16_t i = 0; i < d; ++i) {
    const double w = m.hi[i] - m.lo[i];
    if (!(w > 0.0)) {  // 覆盖 w <= 0 与 NaN
      return 0.0;
    }
    a *= w;
  }
  return a;
}

}  // namespace

double Mbr::Area() const { return SafeArea(*this); }

double Mbr::Margin() const {
  const uint16_t d = lo.dim();
  double s = 0.0;
  for (uint16_t i = 0; i < d; ++i) {
    s += (hi[i] - lo[i]);
  }
  return s;
}

double Mbr::DistanceTo(const Point& p) const {
  const uint16_t d = lo.dim();
  double sum = 0.0;
  for (uint16_t i = 0; i < d; ++i) {
    double diff = 0.0;
    if (p[i] < lo[i]) {
      diff = lo[i] - p[i];
    } else if (p[i] > hi[i]) {
      diff = p[i] - hi[i];
    }
    sum += diff * diff;
  }
  return std::sqrt(sum);
}

bool Mbr::Intersects(const Mbr& other) const {
  const uint16_t d = lo.dim();
  if (d == 0 || other.lo.dim() != d) {
    return false;
  }
  for (uint16_t i = 0; i < d; ++i) {
    if (lo[i] > other.hi[i] || other.lo[i] > hi[i]) {
      return false;
    }
  }
  return true;
}

bool Mbr::Contains(const Mbr& other) const {
  const uint16_t d = lo.dim();
  if (d == 0 || other.lo.dim() != d) {
    return false;
  }
  for (uint16_t i = 0; i < d; ++i) {
    if (other.lo[i] < lo[i] || other.hi[i] > hi[i]) {
      return false;
    }
  }
  return true;
}

bool Mbr::ContainsPoint(const Point& p) const {
  const uint16_t d = lo.dim();
  if (d == 0 || p.dim() != d) {
    return false;
  }
  for (uint16_t i = 0; i < d; ++i) {
    if (p[i] < lo[i] || p[i] > hi[i]) {
      return false;
    }
  }
  return true;
}

void Mbr::ExpandToInclude(const Mbr& other) {
  const uint16_t d = lo.dim();
  for (uint16_t i = 0; i < d; ++i) {
    if (other.lo[i] < lo[i]) {
      lo[i] = other.lo[i];
    }
    if (other.hi[i] > hi[i]) {
      hi[i] = other.hi[i];
    }
  }
}

void Mbr::ExpandToInclude(const Point& p) {
  const uint16_t d = lo.dim();
  for (uint16_t i = 0; i < d; ++i) {
    if (p[i] < lo[i]) {
      lo[i] = p[i];
    }
    if (p[i] > hi[i]) {
      hi[i] = p[i];
    }
  }
}

double Mbr::EnlargementToInclude(const Mbr& other) const {
  const uint16_t d = lo.dim();
  if (d == 0 || d != other.lo.dim() || !valid()) {
    // 非法/空 MBR 无法谈"增量"：返回 0 并让调用方先做 Validate()
    return 0.0;
  }
  double before = 1.0;
  double after = 1.0;
  for (uint16_t i = 0; i < d; ++i) {
    const double lo_i = std::min(lo[i], other.lo[i]);
    const double hi_i = std::max(hi[i], other.hi[i]);
    before *= (hi[i] - lo[i]);
    after *= (hi_i - lo_i);
  }
  return after - before;
}

// ---------------------------------------------------------------------------
// IndexEntry
// ---------------------------------------------------------------------------
Status IndexEntry::Validate() const {
  if (dim() == 0) {
    return Status::Ok();  // 纯一维记录：合法的退化形式
  }
  return mbr.Validate();
}

// ---------------------------------------------------------------------------
// PayloadCodec
// ---------------------------------------------------------------------------
namespace {

template <class T>
void Append(std::vector<char>& buf, const T& v) {
  const char* p = reinterpret_cast<const char*>(&v);
  buf.insert(buf.end(), p, p + sizeof(T));
}

template <class T>
bool Read(const char* data, uint32_t len, uint32_t* cursor, T* out) {
  if (*cursor + sizeof(T) > len) {
    return false;
  }
  std::memcpy(out, data + *cursor, sizeof(T));
  *cursor += sizeof(T);
  return true;
}

}  // namespace

uint32_t PayloadCodec::EncodedSize(const IndexEntry& entry) {
  const uint16_t d = entry.dim();
  return static_cast<uint32_t>(sizeof(uint16_t) + sizeof(uint64_t) + d * 2 * sizeof(double) +
                               sizeof(uint32_t) + entry.payload.size());
}

std::vector<char> PayloadCodec::Encode(const IndexEntry& entry) {
  std::vector<char> buf;
  buf.reserve(EncodedSize(entry));
  const uint16_t d = entry.dim();
  Append(buf, d);
  Append(buf, entry.key);
  for (uint16_t i = 0; i < d; ++i) {
    Append(buf, entry.mbr.lo[i]);
  }
  for (uint16_t i = 0; i < d; ++i) {
    Append(buf, entry.mbr.hi[i]);
  }
  const uint32_t plen = static_cast<uint32_t>(entry.payload.size());
  Append(buf, plen);
  buf.insert(buf.end(), entry.payload.begin(), entry.payload.end());
  return buf;
}

Status PayloadCodec::Decode(const char* data, uint32_t len, IndexEntry* out) {
  if (data == nullptr || out == nullptr) {
    return InvalidArgument("PayloadCodec::Decode got a null pointer");
  }
  uint32_t cursor = 0;
  uint16_t d = 0;
  if (!Read(data, len, &cursor, &d)) {
    return Status::Error(StatusCode::kCorruption, "payload too short to read the dimension");
  }
  if (d > 64) {
    return Status::Error(StatusCode::kCorruption, "the dimension in the payload is invalid: " + std::to_string(d));
  }
  out->mbr.lo.resize(d);
  out->mbr.hi.resize(d);
  if (!Read(data, len, &cursor, &out->key)) {
    return Status::Error(StatusCode::kCorruption, "payload too short to read the key");
  }
  for (uint16_t i = 0; i < d; ++i) {
    if (!Read(data, len, &cursor, &out->mbr.lo[i])) {
      return Status::Error(StatusCode::kCorruption, "payload too short to read the MBR lo bound");
    }
  }
  for (uint16_t i = 0; i < d; ++i) {
    if (!Read(data, len, &cursor, &out->mbr.hi[i])) {
      return Status::Error(StatusCode::kCorruption, "payload too short to read the MBR hi bound");
    }
  }
  uint32_t plen = 0;
  if (!Read(data, len, &cursor, &plen)) {
    return Status::Error(StatusCode::kCorruption, "payload too short to read the payload length");
  }
  if (cursor + plen > len) {
    return Status::Error(StatusCode::kCorruption, "payload length exceeds the record boundary");
  }
  out->payload.assign(data + cursor, data + cursor + plen);
  return Status::Ok();
}

// ---------------------------------------------------------------------------
// RecordRef 的所有权
// ---------------------------------------------------------------------------
RecordRef WithOwnedPayload(RecordRef ref) {
  if (ref.payload == nullptr || ref.payload_len == 0) {
    // 空载荷（合法：调用方只关心键的存在性）：没有可拷贝的东西。
    return ref;
  }
  // 先取 data() 再 move：move 的是 shared_ptr，string 对象本身在堆上不动，
  // 所以 payload 指向的地址在 move 之后依然有效。
  auto buf = std::make_shared<std::string>(ref.payload, ref.payload_len);
  ref.payload = buf->data();
  ref.owned_payload = std::move(buf);
  return ref;
}

}  // namespace ltm
