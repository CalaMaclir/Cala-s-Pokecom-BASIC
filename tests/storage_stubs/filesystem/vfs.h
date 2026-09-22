#pragma once
struct filesystem_t;
struct blockdevice_t;
int fs_mount(const char*, filesystem_t*, blockdevice_t*);
int fs_unmount(const char*);
