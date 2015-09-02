#ifndef __EXT4_PROJ_QUOTA_H__
#define __EXT4_PROJ_QUOTA_H__

extern bool ext4_support_project(const char *device, const char *fstype,
				 const char *root_path);

extern int ext4_create_project(const char *device,
			       const char *path,
			       unsigned long long max_bytes,
			       unsigned long long max_inodes);

/* Use statfs(path, struct statfs) to get current limits and usage */

extern int ext4_resize_project(const char *device,
			       const char *path,
			       unsigned long long max_bytes,
			       unsigned long long max_inodes);

extern int ext4_destroy_project(const char *device, const char *path);

#endif /* __EXT4_PROJ_QUOTA_H__ */
