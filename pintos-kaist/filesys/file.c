#include "filesys/file.h"
#include <debug.h>
#include "filesys/inode.h"
#include "threads/malloc.h"

/* An open file. */
struct file
{
	struct inode *inode; /* File's inode. */
	off_t pos;			 /* Current position. */
	bool deny_write;	 /* Has file_deny_write() been called? */
	int dup_count;		 /* extra2 */
};

/* Opens a file for the given INODE, of which it takes ownership,
 * and returns the new file.  Returns a null pointer if an
 * allocation fails or if INODE is null. */

void increase_dup_count(struct file *file)
{
	file->dup_count++;
}

void decrease_dup_count(struct file *file)
{
	file->dup_count--;
}

int check_dup_count(struct file *file)
{
	return file->dup_count;
}

/*
주어진 inode를 가진 file 구조체를 만들어 반환 .
*/
struct file *
file_open(struct inode *inode)
{
	struct file *file = calloc(1, sizeof *file);
	if (inode != NULL && file != NULL)
	{
		file->inode = inode;
		file->pos = 0;
		/* extra 2 */
		file->dup_count = 1;
		file->deny_write = false;
		return file;
	}
	else
	{
		inode_close(inode);
		free(file);
		return NULL;
	}
}

/* Opens and returns a new file for the same inode as FILE.
 * Returns a null pointer if unsuccessful. */
struct file *
file_reopen(struct file *file)
{
	return file_open(inode_reopen(file->inode));
}

/* Duplicate the file object including attributes and returns a new file for the
 * same inode as FILE. Returns a null pointer if unsuccessful. */
struct file *
file_duplicate(struct file *file)
{
	struct file *nfile = file_open(inode_reopen(file->inode));
	if (nfile)
	{
		nfile->pos = file->pos;
		if (file->deny_write)
			file_deny_write(nfile);
	}
	return nfile;
}

/* Closes FILE. */
void file_close(struct file *file)
{
	if (file != NULL)
	{
		file_allow_write(file);
		inode_close(file->inode);
		free(file);
	}
}

/* Returns the inode encapsulated by FILE. */
struct inode *
file_get_inode(struct file *file)
{
	return file->inode;
}

/* 파일(FILE)에서 현재 위치부터 SIZE 바이트를 버퍼(BUFFER)로 읽어옵니다.
 * 실제로 읽은 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * 파일의 현재 위치(FILE->pos)는 읽은 바이트 수만큼 앞으로 이동합니다. */
off_t file_read(struct file *file, void *buffer, off_t size)
{
	off_t bytes_read = inode_read_at(file->inode, buffer, size, file->pos);
	file->pos += bytes_read;
	return bytes_read;
}

/* 파일(FILE)에서 오프셋(FILE_OFS)부터 SIZE 바이트를 버퍼(BUFFER)로 읽어옵니다.
 * 실제로 읽은 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * 파일의 현재 위치는 영향을 받지 않습니다. */
off_t file_read_at(struct file *file, void *buffer, off_t size, off_t file_ofs)
{
	return inode_read_at(file->inode, buffer, size, file_ofs);
}

/* 버퍼(BUFFER)의 데이터를 파일(FILE)에 현재 위치부터 SIZE 바이트만큼 기록합니다.
 * 실제로 기록된 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * (일반적으로 이 경우 파일을 확장해야 하지만, 파일 확장은 아직 구현되지 않았습니다.)
 * 파일의 현재 위치(FILE->pos)는 기록된 바이트 수만큼 앞으로 이동합니다. */
off_t file_write(struct file *file, const void *buffer, off_t size)
{
	off_t bytes_written = inode_write_at(file->inode, buffer, size, file->pos);
	file->pos += bytes_written;
	return bytes_written;
}

/* 버퍼(BUFFER)의 데이터를 파일(FILE)에 파일 오프셋(FILE_OFS)부터 SIZE 바이트만큼 기록합니다.
 * 실제로 기록된 바이트 수를 반환하며, 파일 끝에 도달하면 SIZE보다 적을 수 있습니다.
 * (일반적으로 이 경우 파일을 확장해야 하지만, 파일 확장은 아직 구현되지 않았습니다.)
 * 파일의 현재 위치는 영향을 받지 않습니다. */
off_t file_write_at(struct file *file, const void *buffer, off_t size,
					off_t file_ofs)
{
	return inode_write_at(file->inode, buffer, size, file_ofs);
}

/* Prevents write operations on FILE's underlying inode
 * until file_allow_write() is called or FILE is closed. */
void file_deny_write(struct file *file)
{
	ASSERT(file != NULL);
	if (!file->deny_write)
	{
		file->deny_write = true;
		inode_deny_write(file->inode);
	}
}

/* Re-enables write operations on FILE's underlying inode.
 * (Writes might still be denied by some other file that has the
 * same inode open.) */
void file_allow_write(struct file *file)
{
	ASSERT(file != NULL);
	if (file->deny_write)
	{
		file->deny_write = false;
		inode_allow_write(file->inode);
	}
}

/* Returns the size of FILE in bytes. */
off_t file_length(struct file *file)
{
	ASSERT(file != NULL);
	return inode_length(file->inode);
}

/* Sets the current position in FILE to NEW_POS bytes from the
 * start of the file. */
void file_seek(struct file *file, off_t new_pos)
{
	ASSERT(file != NULL);
	ASSERT(new_pos >= 0);
	file->pos = new_pos;
}

/* Returns the current position in FILE as a byte offset from the
 * start of the file. */
off_t file_tell(struct file *file)
{
	ASSERT(file != NULL);
	return file->pos;
}
