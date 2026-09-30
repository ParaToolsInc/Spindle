#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* closefrom and close_range were added in glibc 2.34 */
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)

static int run_child(int use_closefrom)
{
   int status;
   pid_t pid = fork();
   if (pid == -1) {
      perror("fork");
      return -1;
   }
   if (pid == 0) {
      struct stat st;
      int fd;
      /* Intercepted stat to allow fork child to reconnect */
      stat("/", &st);
      fd = open("/dev/null", O_RDONLY);
      if (use_closefrom) {
         /* closefrom(int lowfd) closes all file descriptors >= lowfd. */
         closefrom(3);
      }
      /* close_range(unsigned int first, unsigned int last, int flags)
       * closes all file descriptors >= first and <= last. */
      else if (close_range(3, UINT_MAX, 0) == -1) {
         perror("close_range");
         _exit(3);
      }
      /* Verify that the file we opened ourselves was closed */
      if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
         fprintf(stderr, "fd %d was not closed\n", fd);
         _exit(2);
      }
      /* Verify that we can still exec after closing fds;
       * if closefrom/close_range closed Spindle's own fds,
       * this will fail. */
      execl("/bin/true", "true", (char *) NULL);
      perror("execl");
      _exit(1);
   }
   if (waitpid(pid, &status, 0) == -1) {
      perror("waitpid");
      return -1;
   }
   return status;
}

int main(void)
{
   int close_range_status, closefrom_status;

   /* close_range gives ENOSYS if the kernel version doesn't support
    * the corresponding syscall.*/
   if (close_range(INT_MAX, INT_MAX, 0) == -1 && errno == ENOSYS) {
      printf("Skipping: ./close_range_test (kernel lacks close_range)\n");
      return 0;
   }

   close_range_status = run_child(0);
   closefrom_status = run_child(1);
   if (close_range_status == 0 && closefrom_status == 0) {
      printf("PASSED.\n");
      return 0;
   }

   printf("FAILED.\n");
   if (close_range_status != 0)
      fprintf(stderr, " close_range: child failed, status=%d\n", close_range_status);
   if (closefrom_status != 0)
      fprintf(stderr, " closefrom: child failed, status=%d\n", closefrom_status);
   return 1;
}

#else

int main(void)
{
   printf("Skipping: ./close_range_test (glibc < 2.34 lacks closefrom/close_range)\n");
   return 0;
}

#endif
