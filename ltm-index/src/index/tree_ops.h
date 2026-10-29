// src/index/tree_ops.h
// "通用"的核心契约（方案 D3 / §4.6）—— 全项目最重要的一个头文件。
//
// 抽象分两半：
//   TreePageOps ：页内局部操作与布局，树与树的差异全部收敛在这里，不含任何 I/O
//   TreeOps     ：跨页算法（下钻 / 分裂传播 / 下溢处理 / latch 顺序），由各树共享
//
// 通用性验收（方案 D3）：
//   新增一个树变体时，src/storage/** 与 src/index/tree_ops.cc 的 diff 必须为 0 行，
//   差异只允许出现在该树自己的文件与 tree_ops.h 的挂钩声明里。
//   由 scripts/check_layering.py 与 tests/index/test_registry.cc 共同保证。
//   措辞边界见下方 v3 修订记录的【诚实边界】：共享的是结构维护半边，
//   查找半边（定位 + 范围/空间/KNN 查询）本就是各树自己的算法。
//
// ---------------------------------------------------------------------------
// 契约修订记录（M2 实现时发现原版不可实现，故在此固化新语义）
//
// v2（当前）相对 v1 的三处修正：
//   1. 键改用**一等类型 ElemKey**，不再用 `const void* key`。
//      原版要求每棵树自己解释一个裸指针，R 树的多维键无法满足（维度是运行期的），
//      且调用方必须准备一个"既像条目又像键"的缓冲区，语义混乱。
//   2. `BorrowFrom` 增加 `donor_slot`：原版只能借"首条目"，但为了维持 B+ 树
//      "父页第 i 键 == 第 i 子页最小键"这条不变量，向**右**兄弟借键时必须借它的
//      **末**条目。做不到就只能在 BorrowFrom 里偷偷调用 EraseEntry —— 那就把
//      跨页算法泄漏进页内算子了。
//   3. 为页内条目提供 `GetKey/GetChild/EraseSlot` 与页元数据 `Level/FirstKey`，
//      使跨页算法能在**不解释条目二进制布局**的前提下完成下钻、分裂传播与合并。
//      这是"跨页算法只写一遍"能成立的前提。
//
// v3（当前，M3 引入 R 树时）—— 三处修正，全部来自"第二个树变体"暴露的问题：
//   1. **父页分隔条目只有一个来源**：`SplitInto` 不再通过出参返回分隔键。
//      它原本是个"双真相"陷阱 —— 页算子返回的分隔键与父页自己从子页算出来的
//      MBR/键可能不一致（R 树尤其明显：父条目要存 MBR 并集，不是标量键）。
//      现在统一由 `BuildSeparator(child)` 从**子页现状**推导，`SplitInto` 只管搬条目。
//   2. **借键的四种情形下沉进页算子**：`BorrowFrom` 增加方位、父页分隔条目与
//      搬出条目三个参数。原签名只能表达"搬供体的第 i 条到本页末尾"，
//      既表达不了"插到开头"，也表达不了 B+ 内部页借键时**重建最左子页哨兵对**
//      的布局手术 —— 而那正是缺陷 #18 的现场。同时修正一处对 B+ 恰好够用、
//      对多维树必错的不对称：原来只刷新接收方或只刷新供体，而多维树两个方向的
//      MBR 并集都会变，必须两边都刷新（由调用方无条件做）。
//   3. **补齐路由/编码/定位挂钩**，使跨页算法能在不解释条目布局的前提下完成
//      插入、删除、分裂与自检。
//
// 【诚实边界】共享的是**结构维护半边**：插入下钻、分裂传播、借键合并、下溢循环、
//   根升降、元数据、`Validate` 骨架、`Destroy`。
//   查找半边**不可共享**：`DescendToLeaf` 每层只走一条子路径，这对插入成立
//   （按判据选唯一的去向），对查找**不成立** —— 一个探测范围可能同时落在多个
//   孩子里，单路径下钻会漏记录且不触发任何不变量报警。因此
//   `Search`/`Erase` 的定位、`RangeScan`/`SpatialQuery`/`Knn` 由各树自己实现。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "index/index_entry.h"
#include "storage/buffer_pool.h"
#include "util/config.h"
#include "util/status.h"
#include "util/types.h"

namespace ltm {

// 页内用于路由的键：固定标量类型。
// 一维树直接用它；多维树（R 树）需要在插入前把 MBR 编码进本字段才能复用本契约，
// 因此 R 树若要接入同一套跨页算法，必须自行完成多维→标量的路由编码。
using ElemKey = uint64_t;

// B+ 树内部页的最左子页使用这个哨兵键占位（它不参与比较）
constexpr ElemKey kMinElemKey = 0;

// ---------------------------------------------------------------------------
// 页内探测结果
// ---------------------------------------------------------------------------
struct ProbeResult {
  bool found = false;             // 命中（叶页找到键；内部页键在页内出现）
  SlotId slot = kInvalidSlot;     // 命中槽，或叶页的应插入位置
  PageId child = kInvalidPageId;  // 内部页需要下钻的子页号
};

// ---------------------------------------------------------------------------
// 跨页算法搬运「页内条目」的载体
//
// 它只是 v2 里 (key, child, value) 三元组的打包 —— `InsertEntry` 的签名本来
// 就是这三个参数。打包的意义在于：让「父页里指向子页的分隔条目」在跨页算法中
// 有一个统一载体，而不必往契约里再塞裸 ElemKey 参数
// （v2 修订记录第 1 条正是因为裸标量键表达不了多维树才改的，不能走回头路）。
//
// 跨页算法只搬运它，**绝不解释它的二进制布局** —— 那是页算子的职责。
// ---------------------------------------------------------------------------
struct TreeEntry {
  ElemKey key = kMinElemKey;      // 路由标签
  PageId child = kInvalidPageId;  // 指向的子页；叶条目为 kInvalidPageId
  std::vector<char> value;        // 随条目一起存储的字节（由页算子编解码）
};

// 下钻的目的：插入要选"最合适接纳"的孩子，查找要选"可能包含"的孩子。
// 两者的判据在多维树里不同（最小面积增量 vs 范围包含/相交），必须分开传。
enum class SlotChoice : uint8_t { kInsert = 0, kSearch = 1 };

// 供体相对接收方的方位。借键的搬运方向与之绑定：
//   借左兄弟 → 搬它的**末**条目、插到本页**开头**；
//   借右兄弟 → 搬它的**首**条目、追加到本页**末尾**。
// 方向搞反会把键序弄反（B+ 缺陷 #18），所以它必须是显式参数。
enum class BorrowDirection : uint8_t { kFromLeft = 0, kFromRight = 1 };

// 叶页插入的结果。刻意用枚举而不是 bool：bool 的两种含义（成功/覆盖）在
// 调用方极易读反，而"覆盖式插入不能增加条目计数"是元数据计数正确性的关键
// （早期版本在这里无条件 ++，重复插入会把计数撑大，Validate 逐条比对时报错）。
enum class LeafInsertResult : uint8_t { kInserted = 0, kOverwrote = 1 };

// 树结构统计（验收与评测共用）
struct TreeStats {
  uint32_t height = 0;
  uint64_t num_pages = 0;
  uint64_t num_entries = 0;
  uint64_t num_splits = 0;
  uint64_t num_merges = 0;
  uint64_t num_borrows = 0;
  double avg_fill_rate = 0.0;     // 平均填充率（不含根页）
  double avg_overlap_area = 0.0;  // 仅 R 树：内部页 MBR 平均重叠面积
  PageId root_page = kInvalidPageId;
  uint16_t order_or_capacity = 0;
  uint16_t dim = 0;
};

// ---------------------------------------------------------------------------
// TreePageOps：页内局部操作（无 I/O）
// ---------------------------------------------------------------------------
class TreePageOps {
 public:
  virtual ~TreePageOps() = default;

  virtual const char* name() const = 0;

  // ---- 页元数据（跨页算法据此判断叶/内部与页容量）----
  virtual bool IsLeaf(const PageHeader& header) const = 0;
  virtual uint8_t Level(const PageHeader& header) const = 0;
  // 页容量（阶数或每页最大条目数）；用于下溢判定与统计
  virtual uint16_t Capacity() const = 0;
  // 页内第一个条目的键；空页返回 kMinElemKey
  virtual ElemKey FirstKey(const PageHeader& header, const char* payload) const = 0;

  // ---- 条目访问（跨页算法不得直接解释条目布局）----
  // 页内二分定位：cmp(entry_ptr, entry_len, key) 必须返回 <0 / 0 / >0
  virtual SlotId LowerBound(const PageHeader& header, const char* payload, ElemKey key) const = 0;
  virtual ElemKey GetKey(const PageHeader& header, const char* payload, SlotId slot) const = 0;
  // 取该条目指向的子页号；对叶条目返回 kInvalidPageId
  virtual PageId GetChild(const PageHeader& header, const char* payload, SlotId slot) const = 0;

  // ---- 页生命周期 ----
  virtual Status InitPage(PageHeader* header, char* payload, bool is_leaf) = 0;

  // ---- 修改 ----
  // 在 at 位置插入 <key, child, payload>（槽序必须保持键升序）
  virtual Status InsertEntry(PageHeader* header, char* payload, SlotId at, ElemKey key,
                             PageId child, const std::vector<char>& value) = 0;
  virtual Status EraseSlot(PageHeader* header, char* payload, SlotId slot) = 0;

  // ---- 空间与结构判定 ----
  virtual bool IsOverflow(const PageHeader& header, const char* payload) const = 0;
  virtual bool IsUnderflow(const PageHeader& header, const char* payload) const = 0;

  // ---- 分裂 ----
  // 把后半部分搬到 new_page，原始页保留前半部分。
  //
  // 【不再返回分隔键】父页要插入的分隔条目一律由 `BuildSeparator(new_page)` 从
  // 新页现状推导。让本方法同时返回一份"分隔键"会形成双真相：它与父页后续
  // 从子页重算出来的值可能不一致（多维树的分隔条目含 MBR 并集，不只是标量键），
  // 一旦漂移就是 I1/I9 被静默破坏。单一来源是唯一安全的选择。
  //
  // 调用方负责：把 BuildSeparator(new_page) 插到父页、维护叶链、设定新页元数据。
  virtual Status SplitInto(PageHeader* header, char* payload, Page* new_page) = 0;

  // ---- 借键与合并 ----
  // 把 donor 的一条条目搬进本页（本页是接收方，donor 是供体）。返回 true 表示成功。
  //
  //   dir        ：donor 在本页的左边还是右边 —— 决定搬哪一条（末条 / 首条）
  //                以及插到哪一端（开头 / 末尾）。方向搞反会把键序弄反。
  //   parent_sep ：父页中原本路由到 donor 的那个分隔条目。B+ 内部页向右借键时
  //                要把它作为**转移键**下压进本页（父页少一个键、本页多一个键）；
  //                叶页与多维树忽略它。
  //   moved_out  ：被搬动的条目（供调用方统计与决定后续同步）。
  //
  // 四种情形（左/右 × 叶/内部）的布局手术全部由页算子自己完成 ——
  // 尤其 B+ 内部页借键要重建"最左子页哨兵对"，那是页算子才知道的槽约定。
  // 维护 I1/I9 的分隔条目刷新由调用方负责，且必须**两边都刷**：
  // 多维树里借键会让接收方与供体的 MBR 并集同时变化。
  virtual bool BorrowFrom(PageHeader* header, char* payload, Page* donor, BorrowDirection dir,
                          const TreeEntry& parent_sep, TreeEntry* moved_out) = 0;
  // 把 donor 的全部条目追加到本页。
  // separator_for_internal 是父页中位于两页之间、原本路由到 donor 的分隔条目键。
  // B+ 内部页合并必须把它下压成为本页新增条目的键（父页少一个键、子页多一个键，
  // 键的总数守恒）；叶页忽略它；多维树内部页也忽略它 —— 多维树的内部页是
  // "N 个条目对 N 个子页"、没有哨兵，合并时那条分隔条目直接删除即可，不下压。
  // 调用方负责修正 next/prev 与回收 donor 页。
  virtual Status MergeFrom(PageHeader* header, char* payload, const Page& donor,
                           ElemKey separator_for_internal) = 0;

  // ---- 自检（DEBUG 与 validate_tree 使用）----
  // 检查页内自洽：槽序升序、键范围、条目可解析
  virtual Status ValidatePage(const PageHeader& header, const char* payload) const = 0;
  // 检查 I1 的局部形态：父页第 i 个键必须等于第 i 个子页的最小键
  virtual Status CheckParentKey(const PageHeader& parent, const char* parent_payload, SlotId i,
                                const Page& child) const = 0;

  // =========================================================================
  // v3 挂钩：跨页算法与页内布局之间的全部接口
  //
  // 设计原则：**语义性挂钩一律纯虚**，不做"看着像通用默认、其实是某棵树的行为"
  // 的实现 —— 那种默认会被下一个树变体静默继承（编译通过、行为按第一棵树走、
  // 连通用性指标也照样过），是最隐蔽的一类退化。只有确实中性的开关才给默认值。
  // =========================================================================

  // ---- 树身份（建新页 / 写元数据槽位用）----
  virtual MetaTreeKind MetaKind() const = 0;
  virtual PageType PageTypeForLevel(uint8_t level) const = 0;

  // ---- 逻辑记录 <-> 页内条目 ----
  // 一条逻辑记录 → 一个可插入页内的条目（含路由标签、子页号与序列化字节）
  virtual Status EncodeEntry(const IndexEntry& entry, TreeEntry* out) const = 0;
  // 页内叶条目 → 对外记录引用。payload 指针指向页内，守卫释放后失效。
  virtual StatusOr<RecordRef> DecodeLeafEntry(const PageHeader& header, const char* payload,
                                              SlotId slot) const = 0;
  // 定位与 probe 对应的槽；不存在返回 kInvalidSlot。
  // 注意：不能假定叶内条目有序 —— 多维树的叶内条目按分裂结果排列，与路由标签无关。
  virtual SlotId FindEntry(const PageHeader& header, const char* payload,
                           const IndexEntry& probe) const = 0;
  // 叶页插入（含重复条目的处理与压实重试）。kPageFull 表示"压实后仍放不下"，
  // 调用方应先分裂再重插，而不是直接失败。
  virtual StatusOr<LeafInsertResult> InsertLeafEntry(PageHeader* header, char* payload,
                                                     const TreeEntry& entry) = 0;

  // ---- 路由 ----
  // 在内部页里选出承载 target 的槽。kInsert 选"最合适接纳"的孩子，
  // kSearch 选"可能包含"的孩子 —— 多维树里这两者判据不同。
  virtual SlotId ChooseChildSlot(const PageHeader& header, const char* payload,
                                 const IndexEntry& target, SlotChoice purpose) const = 0;
  // 在内部页里列出**可能**承载 target 的子槽，按尝试顺序返回。
  //   一维树：恰好一个（默认实现 = { ChooseChildSlot(...) }，对一维树是精确的）。
  //   多维树：所有 MBR 与 target 相交的槽，可能多个 —— 定位与删除必须逐个尝试，
  //           单路径下钻会**静默漏记录**（见顶部【诚实边界】），所以多维树必须覆写。
  // 默认实现取单路径；它之所以能作为默认值，是因为"一维树只有一个可能去向"
  // 是可证明的，而不是"看起来像某棵树的行为"。
  virtual StatusOr<std::vector<SlotId>> CandidateChildSlots(const PageHeader& header,
                                                            const char* payload,
                                                            const IndexEntry& target,
                                                            SlotChoice purpose) const;

  // ---- 父页分隔条目（I1 / I9 的唯一来源）----
  // 从**子页现状**推导父页中指向它的分隔条目。`child` 字段由本方法填好。
  // 这是父页条目的唯一来源：分裂、借键、合并、插入后的刷新全部经它推导，
  // 不允许另有一处"顺手算出来的分隔键"（双真相必然漂移）。
  // 默认实现取子页最小键（一维树的语义）；多维树覆写为 MBR 并集 + 路由标签。
  virtual TreeEntry BuildSeparator(const PageHeader& child_header,
                                   const char* child_payload) const;
  // 把父页第 slot 号槽的**分隔条目**就地改写为 sep（长度不变；两棵树的分隔条目
  // 都是定长的，所以可以原地改写）。调用方必须先 mutable_page() 标脏。
  virtual Status WriteSeparator(PageHeader* parent_header, char* parent_payload, SlotId slot,
                                const TreeEntry& sep) = 0;
  // 父页第 slot 槽当前是否已经等于 sep。跨页算法用它跳过无谓的改写 ——
  // 这一步不是优化而是**行为保真**：无条件改写会把本来干净的父页标脏，
  // 从而改变缓冲池的驱逐与写盘次数，M2 记录的性能基线就复现不出来了。
  virtual bool SeparatorEquals(const PageHeader& parent_header, const char* parent_payload,
                               SlotId slot, const TreeEntry& sep) const = 0;
  // 一条条目在页内占用的字节数（含槽目录项）。插入前用它做容量预检：
  // 单条都放不下时要立刻明确报错，而不是改到一半才发现。
  virtual uint32_t EntryBytes(const TreeEntry& entry) const = 0;
  // 单条条目的字节预算上限；返回 0 表示不做预算检查（只要求单条能放进空页）。
  //
  // 为什么需要它：变长条目（多维树的载荷）会让"页在达到 capacity_ 条之前就
  // 先被字节撑满"，于是 IsOverflow（槽数口径）永远不触发、分裂路径形同虚设。
  // 声明了预算的树可以在插入前就拒掉过大的条目，从而保证
  //   「页里放得下 capacity_ 条 ⟺ 字节也放得下」
  // 成立 —— 槽数口径的溢出/下溢/合并算术随之对变长条目也正确。
  virtual uint32_t EntryByteBudget() const { return 0; }
  // 在父页 at 处插入一个指向子页的条目。默认实现是"at==0 且本树有最左哨兵时，
  // 键替换为 kMinElemKey"—— 这一条恰好等价于 B+ 根分裂的两次 InsertEntry，
  // 且对没有哨兵的树自动退化为普通插入。
  virtual Status InsertChildAt(PageHeader* header, char* payload, SlotId at,
                               const TreeEntry& entry);
  // 「空间先满」协议（插入 → kPageFull → 分裂 → 选半 → 重插）的第三步：
  // 分裂之后把待插入的条目放进哪一半。返回 left 或 right；
  // 两半都放不下时返回 kInvalidPageId，由调用方报出明确错误。
  //
  // 默认实现是**中性**的（能放进左半就放左半，否则放右半）—— 对任何树都成立，
  // 只是不保证聚类质量。多维树必须覆写为"最小面积增量"，否则分裂出来的两半
  // 会退化成随机划分，R 树的空间聚集性荡然无存。
  virtual PageId ChooseHalf(const TreeEntry& entry, PageId left, const PageHeader& left_header,
                            const char* left_payload, PageId right, const PageHeader& right_header,
                            const char* right_payload) const;

  // ---- 行为开关（都是有明确中性默认的量）----
  // 下溢阈值（槽数口径）。借键时用它判断"借走一条之后供体是否仍然不下溢"：
  // 供体只有在 num_slots > 阈值 时才有多余量可借。
  virtual uint16_t UnderflowThreshold() const;
  // 叶页是否用 next_page/prev_page 串成链（影响 ScanAll 与 Validate 的叶链检查）
  virtual bool HasLeafChain() const { return true; }
  // 内部页 0 号槽是否为"最左子页"哨兵（B+ 是；多维树 0 号槽是真条目）
  virtual bool HasLeftmostSentinel() const { return false; }
  // 插入是否会改变父页的分隔信息。
  //   true  → 跨页算法在插入返回路径上**逐层**刷新分隔条目（分裂与非分裂都要，
  //           分裂后两半都要，且在下钻重插**之后**），因为多维树的分隔条目是
  //           MBR 并集，任何一次插入都会让它变大。
  //   false → 一次也不刷新（一维树的分隔键在既定路由下不会因插入而改变）。
  // 默认 false 是刻意的：若某棵树忘了覆写，症状是 I1/I9 在 Validate 里**大声报错**，
  // 而不是悄悄多出每层一次抓取、把性能基线悄悄改掉。
  virtual bool SeparatorMayChangeOnInsert() const { return false; }
  // 该槽是否为墓碑（已删除但未压实）。墓碑不是条目，计数与键序检查都要跳过。
  virtual bool IsTombstone(const PageHeader& header, const char* payload, SlotId slot) const;
  // 叶内条目是否按键有序。有序才能做二分与"键升序"断言；多维树不保证有序。
  virtual bool KeysAreOrdered() const { return true; }
  // 自检期为多维树累加"内部页 MBR 重叠面积"（默认不动）
  virtual void AccumulateOverlap(const PageHeader& header, const char* payload, double* sum,
                                 uint64_t* count) const;
};

// ---------------------------------------------------------------------------
// TreeOps：树的对外行为（各树实现；跨页通用算法见 §通用性验收）
// ---------------------------------------------------------------------------
class TreeOps {
 public:
  virtual ~TreeOps() = default;

  virtual const char* name() const = 0;

  // 打开/新建：从元数据页读取根页号与高度，或初始化一棵空树
  virtual Status Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
                      TreeId tree_id) = 0;

  virtual Status Insert(const IndexEntry& entry) = 0;
  virtual Status Erase(const IndexEntry& entry) = 0;

  // 精确查找：命中返回 RecordRef。
  //
  // 【载荷所有权，C1 修正】这里**返回的是自持载荷**：Search 在内部守卫析构之前
  // 把字节拷进 `RecordRef::owned_payload`，所以返回值可以安全地带出调用、跨线程
  // 使用。此前契约写的是"payload 在守卫释放后失效，调用方自行拷贝"，但调用方
  // **拿到指针时守卫已经放掉了**，根本没有安全的拷贝窗口 —— 并发淘汰下就是
  // use-after-free（c0_boundary_probe 实测到"读回别人的载荷"）。
  // P1：范围/空间扫描按页复制命中记录并释放页闩后执行 emit；KNN 的载荷由
  // 查询结果缓冲持有。通用回调结果须在回调内物化，或确认 owns_payload()
  // 为真后复制 RecordRef，不能假定回调持有页闩或所有回调结果均有自持载荷。
  virtual StatusOr<RecordRef> Search(const IndexEntry& probe) = 0;

  // 一维区间扫描
  virtual Status RangeScan(const Query& query,
                           const std::function<bool(const RecordRef&)>& emit) = 0;

  // 空间查询（相交 / 包含）；默认 kNotSupported
  virtual Status SpatialQuery(const Query& query,
                              const std::function<bool(const RecordRef&)>& emit) {
    LTM_UNUSED(query);
    LTM_UNUSED(emit);
    return Status::Error(StatusCode::kNotSupported, std::string(name()) + " does not support spatial queries");
  }

  // KNN：默认 kNotSupported，仅 R 树实现
  virtual Status Knn(const Query& query, const std::function<bool(const RecordRef&, double)>& emit) {
    LTM_UNUSED(query);
    LTM_UNUSED(emit);
    return Status::Error(StatusCode::kNotSupported, std::string(name()) + " does not support KNN queries");
  }

  // 全量遍历（验收脚本与差分测试的基石）
  virtual Status ScanAll(const std::function<bool(const RecordRef&)>& emit) = 0;

  // 不变量全树自检：对应方案 §5.3 的 I1..I4、I9
  virtual Status Validate() = 0;

  virtual TreeStats stats() const = 0;

  // 结构销毁：释放全部页回空闲链，元数据页回到初始状态
  virtual Status Destroy() = 0;
};

// ---------------------------------------------------------------------------
// ScopedWriteOp：把一次结构修改包进**一个** WAL 事务（M4-2 的 D1）
//
// 【为什么必须是 RAII，而不是手工配对】Insert/Erase/Destroy 里有十几处
// LTM_RETURN_IF_ERROR 提前返回。逐个补 Commit 必漏一处，而漏掉的后果不是
// "少写点日志"：写事务是可重入的深度计数，漏一次 Commit/Abort 会让 depth 停在
// >0 —— **下一次操作会 join 进这个早已结束的事务**，delta 全部记到错误的事务号
// 上，而且这个错误会一直传染下去。析构兜底是唯一能覆盖全部返回路径的写法。
//
// 语义：
//   * 构造即开启；可重入 —— 已有活跃事务（如 RTree::Insert 内部的 Erase+Insert、
//     以及将来 HybridIndex 的双树 Put）就加入它，不另开一个；
//   * 成功路径**必须**显式 Commit()，只有它写 kCommit 记录并 fsync；
//     注意别写成 `return tree.Insert(...)` —— 内层 Commit 只把深度减回去，
//     真正提交要等外层，直接 return 会让析构把它当失败而 Abort 掉；
//   * 析构时尚未 Commit 则 Abort（包括"记录没找到"这类什么都没改的正常分支：
//     多一条 kAbort 记录，代价可忽略）；
//   * 开启失败时 Commit() 返回那个失败并原样上报，不会静默降级成"没日志也照写"。
// ---------------------------------------------------------------------------
class ScopedWriteOp {
 public:
  explicit ScopedWriteOp(BufferPool* pool);
  ~ScopedWriteOp();
  ScopedWriteOp(const ScopedWriteOp&) = delete;
  ScopedWriteOp& operator=(const ScopedWriteOp&) = delete;

  Status Commit();
  void Abort();
  bool active() const { return pool_ != nullptr; }

 private:
  BufferPool* pool_ = nullptr;
  Status begin_ = Status::Ok();
  bool finished_ = false;
};

// ---------------------------------------------------------------------------
// IndexTree：跨页通用算法（结构维护半边），只写一遍
//
// 各树（B+ / R）只提供 TreePageOps 的页内算子，再套一层薄门面把 TreeOps 的
// 查询接口转接到自己的算法上。新增树变体时本类不需要任何改动 ——
// 这正是 D3 指标要证明的事。
//
// 共享范围（与 tree_ops.h 顶部的【诚实边界】一致）：
//   Open / 元数据持久化 / Fetch 自报家门校验 / 插入下钻与分裂传播 /
//   借键合并与下溢循环 / 根分裂与根降级 / Validate 骨架 / Destroy / stats
// 不共享：记录的**定位**与范围/空间/KNN 查询 —— 单路径下钻对多维树不成立。
// ---------------------------------------------------------------------------
class IndexTree {
 public:
  IndexTree();
  ~IndexTree();
  IndexTree(const IndexTree&) = delete;
  IndexTree& operator=(const IndexTree&) = delete;

  // 打开或新建。page_ops 的 MetaKind() 决定元数据槽位的树类型。
  Status Open(BufferPool* pool, TreePageOps* page_ops, const TreeBuildOptions& options,
              TreeId tree_id);
  bool opened() const;
  uint16_t capacity() const;

  // ---- 结构维护（两棵树共用）----
  Status Insert(const IndexEntry& entry);
  // 插入并报告这条记录**最终落在哪个叶页**。
  //
  // 用途：混合索引要把 R 树的叶页号写进 B+ 的叶条目（方案 §4.9），而"记录落在
  // 哪一页"是插入路径才知道的事（下钻决定叶页，分裂还可能把它放到新页）。
  // 让调用方插入后再自己找一遍，等于把一次插入的成本翻倍。
  //
  // 报告的是**记录实际所在**的页：走了"分裂后重插"那条路时，它是被选中的那一半。
  // 这对 R 树尤其重要 —— 它是变长条目，正常负载下就会走到字节先满的分裂分支。
  Status InsertReturningLeaf(const IndexEntry& entry, PageId* out_leaf);
  // 删除一条记录。定位走 CandidateChildSlots，因此多维树的"同一条记录可能在
  // 多个候选孩子里"由页算子表达，通用层负责逐个尝试。
  Status Erase(const IndexEntry& entry);
  Status Validate();
  Status Destroy();
  TreeStats stats() const;

  // ---- 供各树自己的查询/删除算法复用的原语 ----
  BufferPool* pool() const;
  TreePageOps* page_ops() const;
  TreeId tree_id() const;
  PageId root() const;
  uint8_t height() const;
  uint64_t num_entries() const;
  const TreeBuildOptions& options() const;
  PageId leftmost_leaf() const;

  // 取页并校验"页必须自报家门"（见 Fetch 的实现注释）
  StatusOr<PageGuard> Fetch(PageId pid);

  // 单路径下钻到叶页。一维树的点查与范围起点直接用它。
  // 【多维树不得用它做查询定位】：每层只走一条子路径，而一个探测范围可能同时
  // 落在多个孩子里，会静默漏记录且不触发任何不变量报警。多维树请用
  // TreePageOps::CandidateChildSlots 自己展开搜索。
  StatusOr<PageGuard> DescendToLeaf(const IndexEntry& target, SlotChoice purpose);

  // 一条记录编码成条目后能否放进容量为 capacity_ 的页里。
  // 放不下就明确报错，而不是等到插入到一半才发现 —— 后者会在页已经改动之后
  // 失败，留下半完成状态。
  Status CheckEntryFits(const TreeEntry& entry) const;

  // 从子页现状推导父页应持有的分隔条目（不落盘，只计算）
  StatusOr<TreeEntry> SeparatorOf(PageId child_pid);
  // 用子页现状刷新父页里**所有**指向它的分隔条目。
  // 这是 I1（一维）与 I9（多维，MBR 精确并集）的唯一维护点。
  Status SyncSeparatorForChild(PageId parent_pid, PageId child_pid);
  // 叶页删除一个槽（含压实与条目计数），并报告该叶页是否因此下溢
  Status EraseLeafSlot(PageId leaf_pid, SlotId slot, bool* underflow_out);
  // 让父页 child_slot 槽指向的子页不再下溢：循环借键，借不动则合并
  Status Rebalance(PageId parent_pid, SlotId child_slot);

  struct PageStatsAccumulator {
    double overlap_sum = 0.0;
    uint64_t overlap_count = 0;
  };

 private:
  // 全部状态与私有算法都藏在这里：跨页算法的内部步骤（递归插入、下溢循环、
  // 分隔条目刷新）不应该成为门面可见的接口，否则各树会绕过门面直接调用它们，
  // 共享层就退化成了"谁都能改的公共代码"。
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ltm
