/* C glue for the std.osthreads unit: a shared-nothing binding to the platform
   threads library (pthreads). The unit hands this layer a Kobold routine as a
   function pointer; the glue runs it on a real OS thread. Only scalars cross the
   boundary. Kobold holds no pointers, so a thread is named by an integer id into a
   fixed table, never an address. */
#include <pthread.h>
#include <unistd.h>

#define KP_OSTHREAD_MAX 256

struct kp_osthread_slot {
    pthread_t th;
    int (*body)(int);
    int arg;
    int result;
    int used;
};

static struct kp_osthread_slot kp_osthread_tab[KP_OSTHREAD_MAX];
static pthread_mutex_t kp_osthread_lock = PTHREAD_MUTEX_INITIALIZER;

/* The pthread entry: adapt void *(void *) to the Kobold int(int) worker. */
static void *
kp_osthread_tramp(void *p)
{
    struct kp_osthread_slot *s = (struct kp_osthread_slot *)p;

    s->result = s->body(s->arg);
    return 0;
}

/* Start body(arg) on a real OS thread. Returns a thread id (a table index >= 0),
   or -1 if the table is full or the thread cannot be created. */
int
kp_osthread_spawn(int (*body)(int), int arg)
{
    int i, id = -1;

    pthread_mutex_lock(&kp_osthread_lock);
    for (i = 0; i < KP_OSTHREAD_MAX; i++)
        if (!kp_osthread_tab[i].used) {
            kp_osthread_tab[i].used = 1;
            kp_osthread_tab[i].body = body;
            kp_osthread_tab[i].arg = arg;
            kp_osthread_tab[i].result = 0;
            id = i;
            break;
        }
    pthread_mutex_unlock(&kp_osthread_lock);
    if (id < 0)
        return -1;

    if (pthread_create(&kp_osthread_tab[id].th, 0, kp_osthread_tramp,
                       &kp_osthread_tab[id]) != 0) {
        pthread_mutex_lock(&kp_osthread_lock);
        kp_osthread_tab[id].used = 0;
        pthread_mutex_unlock(&kp_osthread_lock);
        return -1;
    }
    return id;
}

/* Wait for a thread and return its worker's result, reclaiming the id. Returns -1
   for an id that is out of range or not running. */
int
kp_osthread_join(int id)
{
    int r;

    if (id < 0 || id >= KP_OSTHREAD_MAX || !kp_osthread_tab[id].used)
        return -1;
    pthread_join(kp_osthread_tab[id].th, 0);
    r = kp_osthread_tab[id].result;
    pthread_mutex_lock(&kp_osthread_lock);
    kp_osthread_tab[id].used = 0;
    pthread_mutex_unlock(&kp_osthread_lock);
    return r;
}

/* The number of hardware threads available, or 1 when it cannot be determined. */
int
kp_osthread_hwconc(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n > 0 ? (int)n : 1;
}
