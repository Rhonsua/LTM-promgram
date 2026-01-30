#ifdef __COMMON_DEF_H__
#define __COMMON_DEF_H__

<<<<<<< HEAD

// 引入基础依赖头文件
#include <cstdint>   // 固定宽度整数类型 uint64_t
#include <string>    // 字符串值类型
#include <memory>    // 智能指针
#include <utility>   // 通用工具（如pair，后续键值对可用）

/************************** 全局类型定义 (typedef/using) **************************/
// 键值类型：选用64位无符号整数，适配大数键值、磁盘偏移量等场景
typedef uint64_t KeyType;
// 值类型：选用字符串，适配通用的数值/字符串存储需求
typedef std::string ValueType;
// 偏移量类型：用于B+树叶子节点链表、磁盘页偏移，与KeyType同类型保持统一
typedef uint64_t OffsetType;
// 节点编号类型：标识每个节点的唯一ID，方便调试和磁盘管理
typedef uint64_t NodeIdType;

// 通用智能指针别名，简化代码书写
template <typename T>
using SharedPtr = std::shared_ptr<T>;

template <typename T>
using UniquePtr = std::unique_ptr<T>;

// 键值对类型：B+树核心存储单元（叶子节点用）
using KeyValuePair = std::pair<KeyType, ValueType>;

/************************** 宏定义 - 项目核心配置 **************************/
// B+树的阶数：5阶B+树（可根据需求修改，建议取奇数）
#define BP_ORDER 5
// 推导节点最大键值数：B+树节点键值数 = 阶数 - 1
#define BP_NODE_MAX_KEY    (BP_ORDER - 1)
// 叶子节点最大键值数（与通用节点一致，B+树叶子/内部节点键值数上限相同）
#define BP_LEAF_MAX_KEY    BP_NODE_MAX_KEY
// 内部节点最大键值数
#define BP_INTERNAL_MAX_KEY BP_NODE_MAX_KEY
// 节点最小键值数：B+树分裂/合并阈值，取 ceil(阶数/2) - 1
#define BP_NODE_MIN_KEY    ((BP_ORDER + 1) / 2 - 1)

/************************** 全局常量 - 状态码/无效值 **************************/
// 通用操作状态码
const int kSuccess = 0;    // 操作成功
const int kError = -1;     // 通用错误
const int kNotFound = -2;  // 键值未找到
const int kFull = -3;      // 节点已满（需分裂）
const int kEmpty = -4;     // 节点为空
const int kDuplicate = -5; // 键值重复（插入失败）

// 无效值定义：用于初始化/判空
const KeyType kInvalidKey = 0;                // 无效键值
const OffsetType kInvalidOffset = 0;          // 无效偏移量
const NodeIdType kInvalidNodeId = 0;          // 无效节点ID
const ValueType kInvalidValue = std::string();// 无效值

/************************** 全局枚举 - 项目通用类型 **************************/
// 节点类型：叶子节点 / 内部节点（使用enum class，避免命名冲突，C++11特性）
enum class NodeType : uint8_t {
    LEAF_NODE = 0,     // 叶子节点
    INTERNAL_NODE = 1  // 内部节点
};

// 查找模式：精确查找 / 前缀查找（预留扩展，基础实现用精确查找即可）
enum class SearchMode : uint8_t {
    EXACT = 0,         // 精确查找键值
    PREFIX = 1         // 前缀查找（如字符串键值的前缀匹配）
};

/************************** 核心类前置声明 **************************/
// 避免头文件循环引用，仅声明类名，不包含实现
class BPNode;            // B+树基础节点类
class BPLeafNode;        // B+树叶子节点类（继承BPNode）
class BPInternalNode;    // B+树内部节点类（继承BPNode）
class BPTree;            // B+树主类
class BPTreeIterator;    // B+树迭代器类（叶子节点遍历用，预留扩展）

/************************** 节点智能指针别名 **************************/
// 基于前置声明的类，定义专属智能指针，简化代码
using BPNodePtr = SharedPtr<BPNode>;
using BPLeafNodePtr = SharedPtr<BPLeafNode>;
using BPInternalNodePtr = SharedPtr<BPInternalNode>;
using BPTreePtr = SharedPtr<BPTree>;

/************************** 通用工具函数 - 内联小函数 **************************/
// 键值比较：相等
inline bool isKeyEqual(const KeyType& a, const KeyType& b) {
    return a == b;
}

// 键值比较：a < b
inline bool isKeyLess(const KeyType& a, const KeyType& b) {
    return a < b;
}

// 键值比较：a > b
inline bool isKeyGreater(const KeyType& a, const KeyType& b) {
    return a > b;
}

// 检查键值是否为有效键值
inline bool isKeyValid(const KeyType& key) {
    return key != kInvalidKey;
}

// 检查节点是否达到最小键值数（分裂/合并判断）
inline bool isNodeMinKey(const size_t key_num) {
    return key_num == BP_NODE_MIN_KEY;
}

// 检查节点是否达到最大键值数（插入分裂判断）
inline bool isNodeMaxKey(const size_t key_num) {
    return key_num == BP_NODE_MAX_KEY;
}


=======
>>>>>>> 6a019ed155bc7a146a7af6b4988f053fc5723850
#endif // COMMON_DEF_H__

