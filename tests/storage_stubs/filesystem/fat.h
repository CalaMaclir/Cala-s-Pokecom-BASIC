#pragma once
struct filesystem_t {
    int (*unmount)(filesystem_t*) = nullptr;
};
filesystem_t* filesystem_fat_create();
