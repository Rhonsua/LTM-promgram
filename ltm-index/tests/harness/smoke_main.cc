// tests/harness/smoke_main.cc
// M0 冒烟测试：验证骨架的构建、分层、注册表与 CRC 三条底线。
// 运行的每一条断言都必须独立于尚未实现的存储层。
#include <cstdio>
#include <string>
#include <vector>

#include "index/index_entry.h"
#include "index/index_registry.h"
#include "storage/page.h"
#include "util/config.h"
#include "util/crc32c.h"
#include "util/log.h"
#include "util/status.h"
#include "util/types.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (cond) {
    std::printf("  [ok]   %s\n", what);
  } else {
    std::printf("  [FAIL] %s\n", what);
    ++g_failures;
  }
}

void Section(const char* name) { std::printf("\n== %s ==\n", name); }

}  // namespace

int main(int argc, char** argv) {
  ltm::Logger::Instance().SetLevel(ltm::LogLevel::kWarn);

  ltm::Config cfg = ltm::Config::Default();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    size_t eq = a.find('=');
    if (a.rfind("--", 0) == 0 && eq != std::string::npos) {
      ltm::Status s = cfg.Set(a.substr(2, eq - 2), a.substr(eq + 1));
      if (!s.ok()) {
        std::printf("config error: %s\n", s.ToString().c_str());
        return 2;
      }
    }
  }

  Section("util / status");
  ltm::Status ok = ltm::Status::Ok();
  Check(ok.ok(), "Status::Ok() is ok");
  ltm::Status bad = ltm::NotFound("key=42");
  Check(!bad.ok() && bad.code() == ltm::StatusCode::kNotFound, "NotFound returns the right status code");
  Check(bad.ToString().find("kNotFound") != std::string::npos, "Status::ToString contains the status code name");
  Check(ltm::OkStatus().Propagate(bad).code() == ltm::StatusCode::kNotFound, "Propagate forwards the error");

  Section("util / crc32c");
  const char* kVec = "123456789";
  const uint32_t crc_ref = ltm::Crc32c(kVec, 9);
  Check(crc_ref == 0xE3069283u, "CRC32C(\"123456789\") == 0xE3069283");

  // 增量计算必须与一次性计算完全一致
  ltm::Crc32cHasher h;
  h.Update(kVec, 4);
  h.Update(kVec + 4, 5);
  Check(h.value() == crc_ref, "Crc32cHasher incremental results agree");
  Check(ltm::Crc32c(nullptr, 0) == 0u, "an empty input has CRC 0");
  std::printf("  [info] hardware acceleration: %s\n", ltm::Crc32cUsingHardware() ? "enabled" : "disabled (software table)");

  Section("util / config");
  Check(cfg.Set("pool.pool_frames", "8192").ok(), "set pool.pool_frames");
  Check(cfg.pool.pool_frames == 8192, "pool_frames takes effect");
  Check(cfg.Get("pool.pool_frames") == "8192", "Get reads back identically");
  Check(!cfg.Set("pool.pool_frames", "abc").ok(), "an illegal number is rejected");
  Check(!cfg.Set("no.such.key", "1").ok(), "an unknown key is rejected");
  Check(!cfg.Set("pool.enable_prefetch", "maybe").ok(), "an illegal boolean is rejected");
  Check(cfg.Set("log.level", "debug").ok() && cfg.log_level == ltm::LogLevel::kDebug,
        "log level parsing");

  Section("storage / page (basics before M1-2)");
  std::vector<char> raw(ltm::kPageSize, 0);
  ltm::Page page(raw.data());
  page.Init(7, ltm::PageType::kBPlusLeaf, 1, 0);
  Check(page.header().page_id == 7, "Init writes page_id");
  Check(page.header().free_space == ltm::kPagePayloadSize, "after Init the free space is the whole page payload");
  Check(page.Validate().ok(), "the new page passes Validate (checksum included)");
  Check(page.free_space() > 0, "the new page has free space");

  const char e1[] = "hello-entry";
  ltm::SlotId s0 = ltm::kInvalidSlot;
  Check(page.AppendEntry(e1, sizeof(e1), &s0).ok(), "AppendEntry succeeded");
  Check(s0 == 0 && page.num_slots() == 1, "slot ids and counts are correct");
  auto got = page.Entry(0);
  Check(got.ok() && std::string(got.value(), sizeof(e1)) == std::string(e1, sizeof(e1)),
        "the entry reads back identically");
  Check(page.used_bytes() == sizeof(e1), "used_bytes is accounted correctly");

  // 删除 → 碎片 → 压实
  Check(page.EraseEntry(0).ok(), "EraseEntry succeeded");
  Check(page.frag_bytes() == sizeof(e1), "fragments are counted in frag_bytes after an erase");
  page.Compact();
  Check(page.frag_bytes() == 0 && page.live_slots() == 0, "Compact reclaims fragments");
  Check(page.free_space() == ltm::kPagePayloadSize, "free space is restored after Compact");
  // 页级修改操作（Append/Insert/Erase/Compact）**故意不自动重算校验和**：
  // 校验和由缓冲池在脏页 Unpin 时统一 UpdateChecksum() 重算（一次/页，而非一次/写）。
  // 因此这里必须显式重算后再校验。
  page.UpdateChecksum();
  Check(page.Validate().ok(), "passes Validate after recomputing the checksum");

  // 校验和必须能发现损坏
  ltm::PageHeader* hdr = page.mutable_header();
  hdr->page_id = 999;
  Check(!page.Validate().ok(), "a tampered header is caught by Validate (self-description mismatch)");
  page.Init(7, ltm::PageType::kBPlusLeaf, 1, 0);
  raw[100] = 0x7F;  // 篡改 payload 区但不改 header
  Check(!page.Validate().ok(), "a tampered payload is caught by the checksum");

  Section("storage / page boundaries");
  std::vector<char> raw2(ltm::kPageSize, 0);
  ltm::Page p2(raw2.data());
  p2.Init(1, ltm::PageType::kRTreeLeaf, 2, 0);
  std::vector<char> big(6000, 'x');
  Check(!p2.AppendEntry(big.data(), static_cast<uint16_t>(big.size()), nullptr).ok(),
        "an over-long entry is rejected (kPageFull)");
  Check(p2.num_slots() == 0, "the slot count is unchanged after the rejection");
  Check(p2.AppendEntry(nullptr, 0, nullptr).code() == ltm::StatusCode::kInvalidArgument,
        "a zero-length entry is rejected");

  Section("index / generality registry");
  auto& reg = ltm::IndexRegistry::Instance();
  auto names = reg.List();
  Check(!names.empty(), "the built-in tree variants are registered");
  auto mock = reg.Create("mock", ltm::TreeBuildOptions{});
  Check(mock.ok(), "a mock tree can be created by name");
  auto missing = reg.Create("no-such-tree", ltm::TreeBuildOptions{});
  Check(!missing.ok() && missing.status().code() == ltm::StatusCode::kNotFound,
        "an unregistered name returns kNotFound");

  if (mock.ok()) {
    ltm::TreeOps* tree = mock.value().get();
    Check(tree->Open(nullptr, nullptr, ltm::TreeBuildOptions{}, 5).ok(), "mock tree Open succeeded");
    ltm::IndexEntry e;
    e.key = 42;
    e.payload = {'a', 'b', 'c'};
    Check(tree->Insert(e).ok(), "mock tree Insert succeeded");
    auto found = tree->Search(e);
    Check(found.ok() && found.value().key == 42, "mock tree Search hit");
    Check(tree->stats().num_entries == 1, "mock tree stats are correct");
    uint64_t seen = 0;
    tree->ScanAll([&seen](const ltm::RecordRef&) {
      ++seen;
      return true;
    });
    Check(seen == 1, "mock tree ScanAll visited 1 entry");
    Check(tree->Insert(ltm::IndexEntry{}).ok(), "insert a record with key=0");
    Check(tree->stats().num_entries == 2, "re-inserting distinct keys counts correctly");
  }

  Section("index / serialization round trip");
  ltm::IndexEntry src;
  src.key = 12345;
  src.mbr = ltm::Mbr::Invalid(3);
  src.mbr.lo[0] = 1.5; src.mbr.hi[0] = 2.5;
  src.mbr.lo[1] = -3.0; src.mbr.hi[1] = 4.0;
  src.mbr.lo[2] = 0.0; src.mbr.hi[2] = 0.0;
  src.payload = {'p', 'a', 'y'};
  Check(src.mbr.Validate().ok(), "the constructed MBR is valid");
  std::vector<char> enc = ltm::PayloadCodec::Encode(src);
  Check(enc.size() == ltm::PayloadCodec::EncodedSize(src), "EncodedSize agrees with Encode");
  ltm::IndexEntry dec;
  ltm::Status ds = ltm::PayloadCodec::Decode(enc.data(), static_cast<uint32_t>(enc.size()), &dec);
  Check(ds.ok(), "Decode succeeded");
  Check(dec.key == src.key && dec.dim() == 3 && dec.payload == src.payload,
        "key/dim/payload survive the round trip");
  Check(dec.mbr.lo[1] == -3.0 && dec.mbr.hi[2] == 0.0, "MBR coordinates survive the round trip");

  Section("index / MBR math");
  ltm::Mbr a = ltm::Mbr::Invalid(2);
  a.lo[0] = 0; a.hi[0] = 10; a.lo[1] = 0; a.hi[1] = 10;
  ltm::Mbr b = ltm::Mbr::Invalid(2);
  b.lo[0] = 5; b.hi[0] = 15; b.lo[1] = 5; b.hi[1] = 15;
  Check(a.Intersects(b), "the intersect predicate is correct");
  Check(!a.Contains(b), "the contains predicate is correct");
  Check(a.Area() == 100.0, "the area computation is correct");
  Check(a.EnlargementToInclude(b) == 125.0, "the area enlargement is correct (15x15 - 10x10)");
  ltm::Point p(2);
  p[0] = 30; p[1] = 0;
  Check(a.DistanceTo(p) == 20.0, "the point-to-MBR distance is correct");

  Section("current state of the storage and index layers");
  ltm::TreeBuildOptions opts;
  auto bplus = reg.Create("bplus", opts);
  Check(bplus.ok(), "a bplus tree can be created");
  if (bplus.ok()) {
    // 未注入 BufferPool 时必须明确拒绝，而不是静默按空指针运行
    ltm::Status s = bplus.value()->Open(nullptr, nullptr, opts, 1);
    Check(s.code() == ltm::StatusCode::kInvalidArgument, "bplus::Open reports kInvalidArgument when a dependency is missing");
  }
  // R 树的同项检查在 tests/index/test_tree_ops_contract.cc 里（那里包含了 rtree.h）

  std::printf("\n=========================================\n");
  if (g_failures == 0) {
    std::printf("all smoke tests passed. The skeleton is usable.\n");
    return 0;
  }
  std::printf("%d smoke tests failed.\n", g_failures);
  return 1;
}
