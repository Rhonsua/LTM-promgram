

#ifndef __COMMON_DEF_H__
#define __COMMON_DEF_H__

#include <cstdint>   // 必须包含此头文件

constexpr uint64_t INVALID_KEY = 0;
constexpr uint64_t INVALID_ID = 0;
constexpr uint64_t INVALID_BLOCK = 0;

struct PhyAddr {
	uint64_t block_id = 0;
	uint32_t offset = 0;
	bool isValid() const {
		return block_id != INVALID_BLOCK && offset > 0;
	}
};

#endif // __COMMON_DEF_H__