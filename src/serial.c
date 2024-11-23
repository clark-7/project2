#include <dirent.h> 
#include <stdio.h> 
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <pthread.h>
#include <time.h>

#define THREAD_MAX 19//19 threads + 1 main thread = 20 max threads
#define BUFFER_SIZE 1048576 // 1MB

//structure to store each thread's data
typedef struct{
	int start_idx; //starting index in files that thread will process
	int end_idx; //final index+1 in files that thread will process
	char **files; //array of frames
	char *path; //path to follow to find frame
	FILE *f_out; //file to write out to
	int *total_in; //total amount of bytes in
	int *total_out; //total amount of bytes out
	pthread_mutex_t *lockFile; //mutex lock for the file
	pthread_mutex_t *lockIn; //mutex lock for total_in 
	pthread_mutex_t *lockOut; //mutex lock for total_out
} ThreadData ;

//structure to store each frame
typedef struct{
	int bytes_out; //num of bytes out
	char *buffer; //pointer to compressed data
}ThreadFrame;
// allocate size of thread_frame_storage
ThreadFrame thread_frame_storage[750];

//sort in lexicographical order
int cmp(const void *a, const void *b) {
	return strcmp(*(char **) a, *(char **) b);
}


//function to process indiviudal threads
void *compression(void *arg){
	//create structure data
	ThreadData *data = (ThreadData *)arg;

	//handle all the frames needed per thread
	for(int i= data->start_idx; i < data->end_idx; i++) {
		//allocate size to store frame
		thread_frame_storage[i].buffer = malloc(BUFFER_SIZE);
		thread_frame_storage[i].bytes_out = 0;
		
		//create the path to the frame
		int len = strlen(data->path)+strlen(data->files[i])+2;
		char *full_path = malloc(len*sizeof(char));
		assert(full_path != NULL);
		strcpy(full_path, data->path);
		strcat(full_path, "/");
		strcat(full_path, data->files[i]);

		unsigned char buffer_in[BUFFER_SIZE];
		unsigned char buffer_out[BUFFER_SIZE];

		// load file
		FILE *f_in = fopen(full_path, "r");
		assert(f_in != NULL);
		int nbytes = fread(buffer_in, sizeof(unsigned char), BUFFER_SIZE, f_in);
		fclose(f_in);
		//save total number of bytes coming in
		pthread_mutex_lock(data->lockIn);
		*(data->total_in) += nbytes;
		pthread_mutex_unlock(data->lockIn);

		// zip file - not touched
		z_stream strm;
		int ret = deflateInit(&strm, 9);
		assert(ret == Z_OK);
		strm.avail_in = nbytes;
		strm.next_in = buffer_in;
		strm.avail_out = BUFFER_SIZE;
		strm.next_out = buffer_out;
		ret = deflate(&strm, Z_FINISH);
		assert(ret == Z_STREAM_END);

		// save zipped file to frame structure
		pthread_mutex_lock(data->lockFile);
		thread_frame_storage[i].bytes_out = BUFFER_SIZE - strm.avail_out;
		memcpy(thread_frame_storage[i].buffer, buffer_out, thread_frame_storage[i].bytes_out);
		pthread_mutex_unlock(data->lockFile);
		//save total number of bytes going out
		pthread_mutex_lock(data->lockOut);
		*(data->total_out) += (BUFFER_SIZE - strm.avail_out);
		pthread_mutex_unlock(data->lockOut);
		//free path
		free(full_path);
	}
	return NULL;
}

int main(int argc, char **argv) {
	// time computation header
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	// end of time computation header

	// do not modify the main function before this point!
	assert(argc == 2);

	DIR *d;
	struct dirent *dir;
	char **files = NULL;
	int nfiles = 0;

	d = opendir(argv[1]);
	if(d == NULL) {
		printf("An error has occurred\n");
		return 0;
	}
	//allocate files exponentially
	int capacity = 100;
	files = malloc(capacity * sizeof(char *));
	
	// create sorted list of PPM files
	while ((dir = readdir(d)) != NULL) {
		if(nfiles >= capacity){
			capacity*=2;
			files = realloc(files, capacity * sizeof(char *));
		}
		//files = realloc(files, (nfiles+1)*sizeof(char *));		
		assert(files != NULL);
		int len = strlen(dir->d_name);
		if(dir->d_name[len-4] == '.' && dir->d_name[len-3] == 'p' && dir->d_name[len-2] == 'p' && dir->d_name[len-1] == 'm') {
			files[nfiles] = strdup(dir->d_name);
			assert(files[nfiles] != NULL);
			nfiles++;
		}
	}
	closedir(d);
	//sort files 
	qsort(files, nfiles, sizeof(char *), cmp);

	//create mutex locks for threads
	pthread_mutex_t lockFile = PTHREAD_MUTEX_INITIALIZER; //initalize lock
	pthread_mutex_t lockIn = PTHREAD_MUTEX_INITIALIZER; 
	pthread_mutex_t lockOut = PTHREAD_MUTEX_INITIALIZER; 

	// create a single zipped package with all PPM files in lexicographical order
	int total_in = 0, total_out = 0;
	FILE *f_out = fopen("video.vzip", "w");
	assert(f_out != NULL);
	//find number of files each thread needs to calculate
	int files_per_thread = (nfiles +THREAD_MAX -1)/THREAD_MAX;
	//initialize threads
	pthread_t threads[THREAD_MAX];
	ThreadData thread_data[THREAD_MAX];	
	int num_threads = 0;
	for (int t = 0; t < THREAD_MAX; t++){
		//make sure there is not more threads than necessary
		if((t*files_per_thread < nfiles)){
			//initalize structure thread_data
			thread_data[t].start_idx = t * files_per_thread;
			thread_data[t].end_idx = (((t+1) * files_per_thread));
			//if the final index is > number of files change to the number of files
			if (thread_data[t].end_idx > nfiles) thread_data[t].end_idx = nfiles;			
			thread_data[t].files = files;
			thread_data[t].path = argv[1];
			thread_data[t].f_out = f_out;
			thread_data[t].total_in = &total_in;
			thread_data[t].total_out = &total_out;
			thread_data[t].lockFile = &lockFile;
			thread_data[t].lockIn = &lockIn;
			thread_data[t].lockOut = &lockOut;
			//create threads
			pthread_create(&threads[t], NULL, compression, &thread_data[t]);
			//total number of threads
			num_threads=t;
		}
		
	}
	//wait for all threads to terminate
	for (int t = 0; t <= num_threads; t++){
		pthread_join(threads[t], NULL);
	}
	//after threads finished write compressed frames to file in order
	for (int i = 0; i < nfiles; i++){
		fwrite(&thread_frame_storage[i].bytes_out, sizeof(int), 1, f_out);
		fwrite(thread_frame_storage[i].buffer, sizeof(char), thread_frame_storage[i].bytes_out, f_out);
	}
	
	//close file
	fclose(f_out);

	//calculate and print compression rate
	printf("Compression rate: %.2lf%%\n", 100.0*(total_in-total_out)/total_in);
	int i;

	// release list of files
	for(i=0; i < nfiles; i++)
		free(files[i]);
	free(files);

	for(int i = 0; i < nfiles; i++){
		free(thread_frame_storage[i].buffer);
	}

	// do not modify the main function after this point!

	// time computation footer
	clock_gettime(CLOCK_MONOTONIC, &end);
	printf("Time: %.2f seconds\n", ((double)end.tv_sec+1.0e-9*end.tv_nsec)-((double)start.tv_sec+1.0e-9*start.tv_nsec));
	// end of time computation footer

	return 0;
}
