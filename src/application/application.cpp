#include <application/application.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/msg.h>
#include <math.h>

#include "stak.hpp"

#include <pthread.h>
#include <sched.h>

#include <iopp/iotypes.hpp>
#ifdef __APPLE__
#include <iopp/iotypes.hpp>
#else
#include <sys/inotify.h>
#include <bcm2835.h>
#endif

static volatile sig_atomic_t terminate;
static void *lib_handle;
#define stak_log //

struct stak_state_s{
    int (*init)           ( void );                     // int init           ( void );
    int (*update)         ( float delta );              // int update         ( float delta );
    int (*draw)           ( void );                     // int draw           ( void );
    int (*shutdown)       ( void );                     // int shutdown       ( void );
    int (*shutter_release)     ( void );                // int shutter_release     ( void );
    int (*shutter_press)   ( void );                    // int shutter_press   ( void );
    int (*power_release)       ( void );                // int power_release       ( void );
    int (*power_press)     ( void );                    // int power_press     ( void );
    int (*crank_up)       ( void );                     // int crank_up       ( void );
    int (*crank_down)     ( void );                     // int crank_down     ( void );
    int (*crank_rotated)  ( int amount );               // int crank_rotated  ( int amount );
};

static struct stak_state_s app_state;

#if STAK_ENABLE_DYLIBS
#else
    extern int init();
    extern int shutdown();
    extern int update();
#endif


const int pin_rotary_button = 17;
const int pin_rotary_a = 15;
const int pin_rotary_b = 14;

int value_shutter = 0;
int value_rotary_left = 0;
int value_rotary_right = 0;
int rotary_switch_State = 1;
volatile int last_encoded_value = 0, encoder_value = 0, encoder_delta = 0, encoder_last_delta = 0;

//
// lib_open
//
int lib_open(const char* plugin_name, struct stak_state_s* app_state) {
    char *error;

    lib_handle = dlopen (plugin_name, RTLD_LAZY);
    printf("loading %s\n", plugin_name);
    if (!lib_handle) {
        fputs (dlerror(), stderr);
        exit(1);
    }

    // int (*init)           ( void );
    app_state->init = ( int (*)() ) dlsym(lib_handle, "init");
    if ((error = dlerror()) != NULL)  {
        fputs(error, stderr);
        exit(1);
    }

    // int (*update)         ( float delta );
    app_state->update = ( int (*)(float) ) dlsym(lib_handle, "update");
    if ((error = dlerror()) != NULL) app_state->update = 0;

    // int (*draw)           ( void );
    app_state->draw =  ( int (*)() ) dlsym(lib_handle, "draw");
    if ((error = dlerror()) != NULL) app_state->draw = 0;
    
    // int (*shutter_release)     ( void );
    app_state->shutter_release = ( int (*)() ) dlsym(lib_handle, "shutter_release");
    if ((error = dlerror()) != NULL) app_state->shutter_release = 0;
    
    // int (*shutter_press)   ( void );
    app_state->shutter_press = ( int (*)() ) dlsym(lib_handle, "shutter_press");
    if ((error = dlerror()) != NULL) app_state->shutter_press = 0;

    // int (*power_release)       ( void );
    app_state->power_release = ( int (*)() ) dlsym(lib_handle, "power_release");
    if ((error = dlerror()) != NULL) app_state->power_release = 0;

    // int (*power_press)     ( void );
    app_state->power_press = ( int (*)() ) dlsym(lib_handle, "power_press");
    if ((error = dlerror()) != NULL) app_state->power_press = 0;

    // int (*crank_up)       ( void );
    app_state->crank_up = ( int (*)() ) dlsym(lib_handle, "crank_up");
    if ((error = dlerror()) != NULL) app_state->crank_up = 0;

    // int (*crank_down)     ( void );
    app_state->crank_down = ( int (*)() ) dlsym(lib_handle, "crank_down");
    if ((error = dlerror()) != NULL) app_state->crank_down = 0;

    // int (*crank_rotated)  ( int amount );
    app_state->crank_rotated = ( int (*)(int) ) dlsym(lib_handle, "crank_rotated");
    if ((error = dlerror()) != NULL) app_state->crank_rotated = 0;

    // int (*shutdown)       ( void );
    app_state->shutdown = ( int (*)() ) dlsym(lib_handle, "shutdown");
    if ((error = dlerror()) != NULL)  {
        fputs(error, stderr);
        exit(1);
    }

    return 0;
}

//
// lib_close
//
int lib_close(struct stak_state_s* app_state) {
    if (!lib_handle) {
        printf("No library loaded\n");
    }
    dlclose(lib_handle);
    return 0;
}

//
// update_encoder
//
void* update_encoder(void* arg) {
    const int encoding_matrix[4][4] = {
        { 0,-1, 1, 0},
        { 1, 0, 0,-1},
        {-1, 0, 0, 1},
        { 0, 1,-1, 0}
    };

    bcm2835_gpio_fsel(pin_rotary_a, BCM2835_GPIO_FSEL_INPT);
    bcm2835_gpio_fsel(pin_rotary_b, BCM2835_GPIO_FSEL_INPT);
    // set pin pull up/down status
    bcm2835_gpio_set_pud(pin_rotary_a, BCM2835_GPIO_PUD_UP);
    bcm2835_gpio_set_pud(pin_rotary_b, BCM2835_GPIO_PUD_UP);

    double last_time, current_time, delta_time;
    delta_time = last_time = current_time = stak_core_get_time();
    while(!stak_application_get_is_terminating()) {
        current_time = stak_core_get_time();

        int encoded = (bcm2835_gpio_lev(pin_rotary_a) << 1)
                     | bcm2835_gpio_lev(pin_rotary_b);

        encoder_delta = encoding_matrix[last_encoded_value][encoded];

        encoder_value += encoder_delta;
        last_encoded_value = encoded;

        delta_time = (stak_core_get_time() - current_time);
        //uint64_t sleep_time = std::fmin(16000000L, 16000000L - max(0,delta_time));
        //nanosleep((struct timespec[]){{0, sleep_time}}, NULL);
    }
    return 0;
}

//
// stak_core_get_time
//
/*uint64_t stak_core_get_time() {
    struct timespec timer;
    clock_gettime(CLOCK_MONOTONIC, &timer);
    return (uint64_t) (timer.tv_sec) * 1000000000L + timer.tv_nsec;
}*/
double stak_core_get_time() {
    struct timespec time;
    //clock_gettime(CLOCK_MONOTONIC, &time);
    return ( ( double ) ( ( (uint64_t) time.tv_sec ) * 1000000L + time.tv_nsec / 1000L ) ) / 1000000.0;

    //return (uint64_t) (timer.tv_sec) * 1000000000L + timer.tv_nsec;
}

//
// stak_application_terminate_cb
//
void stak_application_terminate_cb(int signum)
{
    // stak_log("Terminating...%s", "");
    stak_application_terminate();
}

//
// stak_application_create
//
STAK_EXPORT struct stak_application_s* stak_application_create(stak::string plugin_name) {

    struct stak_application_s* application = (stak_application_s*) calloc(1, sizeof(struct stak_application_s));
    application->plugin_name = plugin_name;
    //strcpy( application->plugin_name, plugin_name.c_str() );

#if STAK_ENABLE_SEPS114A
    application->display = stak_seps114a_create();
    application->canvas = stak_canvas_create(STAK_CANVAS_OFFSCREEN, 96, 96);

    if(!bcm2835_init()) {
        printf("Failed to init BCM2835 library.\n");
        stak_application_terminate();
        return 0;
    }
#endif

#if STAK_ENABLE_DYLIBS
    lib_open(application->plugin_name.c_str(), &app_state);
#else
    app_state.init = init;
    app_state.shutdown = shutdown;
    app_state.update = update;
#endif
    if(app_state.init) {
        app_state.init();
    }
    return application;
}

//
// stak_application_destroy
//
STAK_EXPORT int stak_application_destroy(struct stak_application_s* application) {
    // if shutdown method exists, run it
    if(app_state.shutdown) {
        app_state.shutdown();
    }
#if STAK_ENABLE_DYLIBS
    lib_close(&app_state);
#endif

    if(pthread_join(application->thread_hal_update, NULL)) {
        fprintf(stderr, "Error joining thread\n");
        return -1;
    }
#if STAK_ENABLE_SEPS114A
    stak_canvas_destroy(application->canvas);
    stak_seps114a_destroy(application->display);
#endif
    free(application);
    return 0;
}

#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define BUF_LEN     ( 1024 * ( EVENT_SIZE + 16 ) )


//
// stak_application_run
//
STAK_EXPORT int stak_application_run(struct stak_application_s* application) {
    struct sigaction action;
#if STAK_ENABLE_DYLIBS
  #ifndef __APPLE__
    int lib_fd = inotify_init();
    int lib_wd, lib_read_length;
    char lib_notify_buffer[BUF_LEN];

    if( lib_fd < 0 ) {
        perror( "inotify_init" );
        return -1;
    }
    lib_wd = inotify_add_watch( lib_fd, "./build/", IN_CLOSE_WRITE );
    if( lib_wd < 0 ) {
        perror( "inotify_add_watch" );
        return -1;
    }
    int flags = fcntl(lib_fd, F_GETFL, 0);
    if (fcntl(lib_fd, F_SETFL, flags | O_NONBLOCK) == -1 ) {
        perror( "fcntl" );
        close(lib_fd);
        return -1;

    }
  #endif
#endif

    pthread_create(&application->thread_hal_update, NULL, update_encoder, NULL);

        // setup sigterm handler
    action.sa_handler = stak_application_terminate_cb;
    sigaction(SIGINT, &action, NULL);

    //double last_time, current_time, delta_time;
    //delta_time = last_time = current_time = stak_core_get_time();
    int frames_this_second = 0;
    int frames_per_second = 0;
    int rotary_last_value = 0;
    int fps_limit = 60;
    double last_time = stak_core_get_time();
    double current_time = last_time;
    double fps_counter_last_time = current_time;
    double delta_time = current_time - last_time;
    
    double frame_max_time = 1.0 / fps_limit;
    while(!terminate) {
        frames_this_second++;
        current_time = stak_core_get_time();
        last_time = current_time;


        if(current_time > fps_counter_last_time + 1.0f) {
            frames_per_second = frames_this_second;
            frames_this_second = 0;
            fps_counter_last_time = current_time;
            //stak_log("FPS: %i", frames_per_second);
        }

        if(app_state.crank_rotated) {
            if(rotary_last_value != encoder_value) {
                app_state.crank_rotated(rotary_last_value - encoder_value);
                rotary_last_value = encoder_value;
            }
        }

        if( app_state.update ) {
            app_state.update( delta_time );
        }

#if STAK_ENABLE_SEPS114A
    #if 1
            stak_canvas_swap(application->canvas);
            stak_canvas_copy(application->canvas, (uint8_t*)application->display->framebuffer, 96 * 2);
    #endif
    #if 1
        stak_seps114a_update(application->display);
    #endif
#endif

        delta_time = (stak_core_get_time() - current_time);
        uint64_t sleep_time = fmin(frame_max_time, frame_max_time - fmax(0,delta_time)) * 1000000L;
#ifndef __APPLE__
        //nanosleep((struct timespec[]) {{0, sleep_time}}, NULL );
#else
      //nanosleep((struct timespec[]) {{0, (uint32_t)sleep_time}}, NULL );
#endif

#if STAK_ENABLE_DYLIBS
  #ifndef __APPLE__
        stak::string plugin_file_name;

        {
            int start_pos = application->plugin_name.find_last_of( '/', 0 ) + 1;
            int length = application->plugin_name.size() - start_pos;
            //plugin_file_name = malloc(length + 1);
            plugin_file_name = application->plugin_name.substr(start_pos, length);
            //strcpy(plugin_file_name, application->plugin_name + start_pos);
        }

        // read inotify buffer to see if library has been modified
        lib_read_length = read( lib_fd, lib_notify_buffer, BUF_LEN );
        if (( lib_read_length < 0 ) && ( errno == EAGAIN)){
        }
        else if(lib_read_length >= 0) {
            int i = 0;
            //while ( i < lib_read_length ) {
                struct inotify_event *event = ( struct inotify_event * ) &lib_notify_buffer[ i ];
                if ( event->mask & IN_CLOSE_WRITE ) {
                    if ( ( ! (event->mask & IN_ISDIR) ) && ( strstr(event->name, plugin_file_name.c_str() ) != 0 ) ) {

                        // if shutdown method exists, run it
                        if(app_state.shutdown) {
                            app_state.shutdown();
                        }

                        // close currently open lib and reload it
                        lib_close(&app_state);
                        lib_open(application->plugin_name.c_str(), &app_state);

                        // if init method exists, run it
                        if(app_state.init) {
                            app_state.init();
                        }
                    }
                }
                i += EVENT_SIZE + event->len;
            //}
        }
        else {
            perror( "read" );
        }
  #endif
#endif
    }
    return 0;
}

//
// stak_application_terminate
//
STAK_EXPORT int stak_application_terminate() {
    terminate = 1;
    return 0;
}
//
// stak_application_terminate
//
STAK_EXPORT int stak_application_get_is_terminating() {
    return terminate;
}

int error_throw( const char* file, int line, const char* function,const char* string ) {
    printf( "\33[36m[\33[35m %s \33[36;1m@\33[0;33m %4i \33[36m] \33[0;34m %64s \33[31mERROR\33[0;39m %s\n", file, line, function, string );
    return -1;
}

int stak_get_rotary_value( ) {
    return encoder_delta;
}