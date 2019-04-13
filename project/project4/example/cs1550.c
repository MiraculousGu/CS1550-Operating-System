/*
FUSE: Filesystem in Userspace
Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

This program can be distributed under the terms of the GNU GPL.
See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//define boolean variables
#define TRUE 1
#define FALSE 0

//size of .disk
#define DISK_SIZE 5242880

//number of block (except bitmap)
#define BLOCK_COUNT = (DISK_SIZE / BLOCK_SIZE - ((DISK_SIZE - 1) / (8 * BLOCK_SIZE * BLOCK_SIZE) + 1))

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
	//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
	//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;


//operation for disks --- function prototypes
static FILE* open_disk(void);	//open the disk
static void close_disk(FILE *f);	//close the disk
//static int update_bitmap(FILE *f, long block_idx, char val);	//update the bitmap
static cs1550_root_directory read_root(FILE *disk);	//return the first block of .disk
static void* get_block(FILE *f, long block_idx);		//return the whole block


/*
* Called whenever the system wants to know the file attributes, including
* simply whether the file exists or not.
*
* man -s 2 stat will show the fields of a stat structure
*/
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));

	//is path the root dir?
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {

		//Check if name is subdirectory
		/*
		//Might want to return a structure with these fields
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		res = 0; //no error
		*/

		//Check if name is a regular file
		/*
		//regular file, probably want to be read and write
		stbuf->st_mode = S_IFREG | 0666;
		stbuf->st_nlink = 1; //file links
		stbuf->st_size = 0; //file size - make sure you replace with real size!
		res = 0; // no error
		*/

		//implementation
		char directory[MAX_FILENAME + 1];	// + 1 is for nul
		char filename[MAX_FILENAME + 1];
		char extension[MAX_EXTENSION + 1];

		//initialize variables
		strcpy(directory, "");
		strcpy(filename, "");
		strcpy(extension, "");
		sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
		/*
		Comment for %[^\n]:
		the %[^\n] conversion specification, which matches a string of all characters
		not equal to the new line character ('\n') and stores it (plus a terminating
		'\0' character) in str.
		*/

		//check whether the name is valid for our rule
		if(strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION){
			return -ENAMETOOLONG;
		}

		//check if name is sub_directory
		if(strcmp(directory,"") != 0){
			//this means the directory exits
			FILE *disk = open_disk();		//ready to read the disk blocks

			//just sub_directory
			cs1550_root_directory root = read_root(disk);		//read the root folder

			//search for the directory
			int i;
			int result = FALSE;
			struct cs1550_directory dir;
			for(i = 0; i < root.nDirectories; i++){
				//locate the current directory
				struct cs1550_directory current = root.directories[i];
				//compare the name
				if(strcmp(current.dname,directory) == 0){
					//we found this directory, now set up the struct
					result = TRUE;
					dir = current;
					break;
				}
			}

			//if we found the directory
			if(result){

				//then we chech whether it is a file
				if(strcmp(filename,"") != 0){
					//this means it is a file
					//now we need to locate the beginning of this directory
					struct cs1550_directory_entry *entry = get_block(disk, dir.nStartBlock);

					if(entry == NULL){
						perror("some error occurs\n");
						res = -ENOENT;
						return res;
					}

					result = FALSE;		//reset the result
					int num_files = entry->nFiles;	  //numer of files current directory has
					if(num_files > MAX_FILES_IN_DIR){
						perror("too many files or error of reading\n");
					}
					//search for the file
					int j;
					for(j = 0; j < num_files; j++){
						struct cs1550_file_directory cur_file = entry -> files[j];
						//check if the file name matches
						if(strcmp(cur_file.fname, filename) == 0){
							stbuf->st_mode = S_IFREG | 0666;
							stbuf->st_nlink = 1; //file links
							stbuf->st_size = 0; //file size - make sure you replace with real size!
							res = 0;
							result = TRUE;
							break;
						}
					}

					if(!result){
						res = -ENOENT;
						return res;
					}

				}

				else{
					//just the directory
					stbuf->st_mode = S_IFDIR | 0755;
					stbuf->st_nlink = 2;
					res = 0;
				}

			}
			else{
				//Else return that path doesn't exist
				res = -ENOENT;
				return res;
			}
			close_disk(disk);
		}
		else{
			//Else return that path doesn't exist
			res = -ENOENT;
		}
	}
	return res;
}

/*
* Called whenever the contents of a directory are desired. Could be from an 'ls'
* or could even be when a user hits TAB to do autocompletion
*/
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info *fi)
	{
		//Since we're building with -Wall (all warnings reported) we need
		//to "use" every parameter, so let's just cast them to void to
		//satisfy the compiler
		(void) offset;
		(void) fi;

		//This line assumes we have no subdirectories, need to change
		if (strcmp(path, "/") != 0)
		return -ENOENT;

		//the filler function allows us to add entries to the listing
		//read the fuse.h file for a description (in the ../include dir)
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);

		/*
		//add the user stuff (subdirs or files)
		//the +1 skips the leading '/' on the filenames
		filler(buf, newpath + 1, NULL, 0);
		*/
		return 0;
	}

	/*
	* Creates a directory. We can ignore mode since we're not dealing with
	* permissions, as long as getattr returns appropriate ones for us.
	*/
	static int cs1550_mkdir(const char *path, mode_t mode)
	{
		(void) path;
		(void) mode;

		return 0;
	}

	/*
	* Removes a directory.
	*/
	static int cs1550_rmdir(const char *path)
	{
		(void) path;
		return 0;
	}

	/*
	* Does the actual creation of a file. Mode and dev can be ignored.
	*
	*/
	static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
	{
		(void) mode;
		(void) dev;
		return 0;
	}

	/*
	* Deletes a file
	*/
	static int cs1550_unlink(const char *path)
	{
		(void) path;

		return 0;
	}

	/*
	* Read size bytes from file into buf starting from offset
	*
	*/
	static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
		{
			(void) buf;
			(void) offset;
			(void) fi;
			(void) path;

			//check to make sure path exists
			//check that size is > 0
			//check that offset is <= to the file size
			//read in data
			//set size and return, or error

			size = 0;

			return size;
		}

		/*
		* Write size bytes from buf into file starting from offset
		*
		*/
		static int cs1550_write(const char *path, const char *buf, size_t size,
			off_t offset, struct fuse_file_info *fi)
			{
				(void) buf;
				(void) offset;
				(void) fi;
				(void) path;

				//check to make sure path exists
				//check that size is > 0
				//check that offset is <= to the file size
				//write data
				//set size (should be same as input) and return, or error

				return size;
			}


			//implementations of function prototypes

			//open the .disk
			static FILE* open_disk(void){
				//create a file pointer to manage the disk
				FILE *file;
				file  = fopen(".disk", "r+b");
				// Check if disk file is able to be open
				if (file == NULL) {
					perror ("Error opening file\n");
					return NULL;
				}

				// Check disk file size
				if (fseek(file, 0, SEEK_END) || ftell(file) != DISK_SIZE) {
					fclose(file);
					fprintf(stderr, "disk file is not valid\n");
					return NULL;
				}
				return file;
			}

			//close the .disk
			static void close_disk(FILE *f){
				//check if whether the file is closed
				if(fclose(f) != 0){
					perror("close the disk failed\n");
				}
				return;
			}

			//read the root
			//Read root from disk
			static cs1550_root_directory read_root(FILE *disk){
				fseek(disk, 0, SEEK_SET);
				cs1550_root_directory root;
				fread(&root, BLOCK_SIZE, 1, disk);
				return root;
			}

			static void* get_block(FILE *file, long block_idx){
				/*if (!(block_idx < BLOCK_COUNT)) {
					perror("requested block %ld does not exist\n");
					return NULL;
				}*/
				if (fseek(file, block_idx * BLOCK_SIZE, SEEK_SET)) {
					perror("failed to seek to block\n");
					return NULL;
				}
				void *block = malloc(BLOCK_SIZE);
				if (fread(block, BLOCK_SIZE, 1, file) != 1) {
					perror("failed to load block\n");
					free(block);
					return NULL;
				}
				return block;
			}


			/******************************************************************************
			*
			*  DO NOT MODIFY ANYTHING BELOW THIS LINE
			*
			*****************************************************************************/

			/*
			* truncate is called when a new file is created (with a 0 size) or when an
			* existing file is made shorter. We're not handling deleting files or
			* truncating existing ones, so all we need to do here is to initialize
			* the appropriate directory entry.
			*
			*/
			static int cs1550_truncate(const char *path, off_t size)
			{
				(void) path;
				(void) size;

				return 0;
			}


			/*
			* Called when we open a file
			*
			*/
			static int cs1550_open(const char *path, struct fuse_file_info *fi)
			{
				(void) path;
				(void) fi;
				/*
				//if we can't find the desired file, return an error
				return -ENOENT;
				*/

				//It's not really necessary for this project to anything in open

				/* We're not going to worry about permissions for this project, but
				if we were and we don't have them to the file we should return an error

				return -EACCES;
				*/

				return 0; //success!
			}

			/*
			* Called when close is called on a file descriptor, but because it might
			* have been dup'ed, this isn't a guarantee we won't ever need the file
			* again. For us, return success simply to avoid the unimplemented error
			* in the debug log.
			*/
			static int cs1550_flush (const char *path , struct fuse_file_info *fi)
			{
				(void) path;
				(void) fi;

				return 0; //success!
			}

			//register our new functions as the implementations of the syscalls
			static struct fuse_operations hello_oper = {
				.getattr	= cs1550_getattr,
				.readdir	= cs1550_readdir,
				.mkdir	= cs1550_mkdir,
				.rmdir = cs1550_rmdir,
				.read	= cs1550_read,
				.write	= cs1550_write,
				.mknod	= cs1550_mknod,
				.unlink = cs1550_unlink,
				.truncate = cs1550_truncate,
				.flush = cs1550_flush,
				.open	= cs1550_open,
			};

			//Don't change this.
			int main(int argc, char *argv[])
			{
				return fuse_main(argc, argv, &hello_oper, NULL);
			}
