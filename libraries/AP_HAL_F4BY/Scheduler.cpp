/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-

#include <AP_HAL/AP_HAL.h>
#if CONFIG_HAL_BOARD == HAL_BOARD_F4BY

#include "AP_HAL_F4BY.h"
#include "Scheduler.h"

#include <unistd.h>
#include <stdlib.h>
#include <sched.h>
#include <errno.h>
#include <stdio.h>
#include <drivers/drv_hrt.h>
#include <nuttx/arch.h>
#include <systemlib/systemlib.h>
#include <pthread.h>
#include <poll.h>

#include "UARTDriver.h"
#include "AnalogIn.h"
#include "Storage.h"
#include "RCOutput.h"
#include "RCInput.h"
#include <AP_Scheduler/AP_Scheduler.h>

using namespace F4BY;

extern const AP_HAL::HAL& hal;

extern bool _f4by_thread_should_exit;

F4BYScheduler::F4BYScheduler() :
    _perf_timers(perf_alloc(PC_ELAPSED, "APM_timers")),
    _perf_io_timers(perf_alloc(PC_ELAPSED, "APM_IO_timers")),
    _perf_storage_timer(perf_alloc(PC_ELAPSED, "APM_storage_timers")),
	_perf_delay(perf_alloc(PC_ELAPSED, "APM_delay"))
{}

void F4BYScheduler::init()
{
    _main_task_pid = getpid();

    // setup the timer thread - this will call tasks at 1kHz
	pthread_attr_t thread_attr;
	struct sched_param param;

	pthread_attr_init(&thread_attr);
	pthread_attr_setstacksize(&thread_attr, 2048);

	param.sched_priority = APM_TIMER_PRIORITY;
	(void)pthread_attr_setschedparam(&thread_attr, &param);
    pthread_attr_setschedpolicy(&thread_attr, SCHED_FIFO);

	pthread_create(&_timer_thread_ctx, &thread_attr, &F4BYScheduler::_timer_thread, this);

    // the UART thread runs at a medium priority
	pthread_attr_init(&thread_attr);
	pthread_attr_setstacksize(&thread_attr, 2048);

	param.sched_priority = APM_UART_PRIORITY;
	(void)pthread_attr_setschedparam(&thread_attr, &param);
    pthread_attr_setschedpolicy(&thread_attr, SCHED_FIFO);

	pthread_create(&_uart_thread_ctx, &thread_attr, &F4BYScheduler::_uart_thread, this);

    // the IO thread runs at lower priority
	pthread_attr_init(&thread_attr);
	pthread_attr_setstacksize(&thread_attr, 2048);

	param.sched_priority = APM_IO_PRIORITY;
	(void)pthread_attr_setschedparam(&thread_attr, &param);
    pthread_attr_setschedpolicy(&thread_attr, SCHED_FIFO);

	pthread_create(&_io_thread_ctx, &thread_attr, &F4BYScheduler::_io_thread, this);

    // the storage thread runs at just above IO priority
    pthread_attr_init(&thread_attr);
    pthread_attr_setstacksize(&thread_attr, 1024);

    param.sched_priority = APM_STORAGE_PRIORITY;
    (void)pthread_attr_setschedparam(&thread_attr, &param);
    pthread_attr_setschedpolicy(&thread_attr, SCHED_FIFO);

    pthread_create(&_storage_thread_ctx, &thread_attr, &F4BYScheduler::_storage_thread, this);
}

/**
   delay for a specified number of microseconds using a semaphore wait
 */
void F4BYScheduler::delay_microseconds_semaphore(uint16_t usec) 
{
    sem_t wait_semaphore;
    struct hrt_call wait_call;
    sem_init(&wait_semaphore, 0, 0);
    memset(&wait_call, 0, sizeof(wait_call));
    hrt_call_after(&wait_call, usec, (hrt_callout)sem_post, &wait_semaphore);
    sem_wait(&wait_semaphore);
}

void F4BYScheduler::delay_microseconds(uint16_t usec) 
{
    perf_begin(_perf_delay);
    delay_microseconds_semaphore(usec);
    perf_end(_perf_delay);
}

/*
  wrapper around sem_post that boosts main thread priority
 */
static void sem_post_boost(sem_t *sem)
{
    hal_f4by_set_priority(APM_MAIN_PRIORITY_BOOST);
    sem_post(sem);
}

/*
  return the main thread to normal priority
 */
static void set_normal_priority(void *sem)
{
    hal_f4by_set_priority(APM_MAIN_PRIORITY);
}

/*
  a variant of delay_microseconds that boosts priority to
  APM_MAIN_PRIORITY_BOOST for APM_MAIN_PRIORITY_BOOST_USEC
  microseconds when the time completes. This significantly improves
  the regularity of timing of the main loop as it takes 
 */
void F4BYScheduler::delay_microseconds_boost(uint16_t usec) 
{
    sem_t wait_semaphore;
    static struct hrt_call wait_call;
    sem_init(&wait_semaphore, 0, 0);
    hrt_call_after(&wait_call, usec, (hrt_callout)sem_post_boost, &wait_semaphore);
    sem_wait(&wait_semaphore);
    hrt_call_after(&wait_call, APM_MAIN_PRIORITY_BOOST_USEC, (hrt_callout)set_normal_priority, NULL);
}

void F4BYScheduler::delay(uint16_t ms)
{
    if (in_timerprocess()) {
        ::printf("ERROR: delay() from timer process\n");
        return;
    }
    perf_begin(_perf_delay);
	uint64_t start = AP_HAL::micros64();
    
    while ((AP_HAL::micros64() - start)/1000 < ms && 
           !_f4by_thread_should_exit) {
        delay_microseconds_semaphore(1000);
        if (_min_delay_cb_ms <= ms) {
            if (_delay_cb) {
                _delay_cb();
            }
        }
    }
    perf_end(_perf_delay);
    if (_f4by_thread_should_exit) {
        exit(1);
    }
}

void F4BYScheduler::register_delay_callback(AP_HAL::Proc proc,
                                            uint16_t min_time_ms) 
{
    _delay_cb = proc;
    _min_delay_cb_ms = min_time_ms;
}

void F4BYScheduler::register_timer_process(AP_HAL::MemberProc proc) 
{
    for (uint8_t i = 0; i < _num_timer_procs; i++) {
        if (_timer_proc[i] == proc) {
            return;
        }
    }

    if (_num_timer_procs < F4BY_SCHEDULER_MAX_TIMER_PROCS) {
        _timer_proc[_num_timer_procs] = proc;
        _num_timer_procs++;
    } else {
        hal.console->printf("Out of timer processes\n");
    }
}

void F4BYScheduler::register_io_process(AP_HAL::MemberProc proc) 
{
    for (uint8_t i = 0; i < _num_io_procs; i++) {
        if (_io_proc[i] == proc) {
            return;
        }
    }

    if (_num_io_procs < F4BY_SCHEDULER_MAX_TIMER_PROCS) {
        _io_proc[_num_io_procs] = proc;
        _num_io_procs++;
    } else {
        hal.console->printf("Out of IO processes\n");
    }
}

void F4BYScheduler::register_timer_failsafe(AP_HAL::Proc failsafe, uint32_t period_us) 
{
    _failsafe = failsafe;
}

void F4BYScheduler::suspend_timer_procs() 
{
    _timer_suspended = true;
}

void F4BYScheduler::resume_timer_procs() 
{
    _timer_suspended = false;
    if (_timer_event_missed == true) {
        _run_timers(false);
        _timer_event_missed = false;
    }
}

void F4BYScheduler::reboot(bool hold_in_bootloader) 
{
    // disarm motors to ensure they are off during a bootloader upload
    hal.rcout->force_safety_on();
    hal.rcout->force_safety_no_wait();

    // delay to ensure the async force_saftey operation completes
    delay(500);

	px4_systemreset(hold_in_bootloader);
}

void F4BYScheduler::_run_timers(bool called_from_timer_thread)
{
    if (_in_timer_proc) {
        return;
    }
    _in_timer_proc = true;

    if (!_timer_suspended) {
        // now call the timer based drivers
        for (int i = 0; i < _num_timer_procs; i++) {
            if (_timer_proc[i]) {
                _timer_proc[i]();
            }
        }
    } else if (called_from_timer_thread) {
        _timer_event_missed = true;
    }

    // and the failsafe, if one is setup
    if (_failsafe != NULL) {
        _failsafe();
    }

    // process analog input
    ((F4BYAnalogIn *)hal.analogin)->_timer_tick();

    _in_timer_proc = false;
}

extern bool f4by_ran_overtime;

void *F4BYScheduler::_timer_thread(void *arg)
{
    F4BYScheduler *sched = (F4BYScheduler *)arg;
    uint32_t last_ran_overtime = 0;

    while (!sched->_hal_initialized) {
        poll(NULL, 0, 1);        
    }
    while (!_f4by_thread_should_exit) {
        sched->delay_microseconds_semaphore(1000);

        // run registered timers
        perf_begin(sched->_perf_timers);
        sched->_run_timers(true);
        perf_end(sched->_perf_timers);

        // process any pending RC output requests
        ((F4BYRCOutput *)hal.rcout)->_timer_tick();

        // process any pending RC input requests
        ((F4BYRCInput *)hal.rcin)->_timer_tick();

        if (f4by_ran_overtime && AP_HAL::millis() - last_ran_overtime > 2000) {
            last_ran_overtime = AP_HAL::millis();
#if 0
            printf("Overtime in task %d\n", (int)AP_Scheduler::current_task);
            hal.console->printf("Overtime in task %d\n", (int)AP_Scheduler::current_task);
#endif
        }
    }
    return NULL;
}

void F4BYScheduler::_run_io(void)
{
    if (_in_io_proc) {
        return;
    }
    _in_io_proc = true;

    if (!_timer_suspended) {
        // now call the IO based drivers
        for (int i = 0; i < _num_io_procs; i++) {
            if (_io_proc[i]) {
                _io_proc[i]();
            }
        }
    }

    _in_io_proc = false;
}

void *F4BYScheduler::_uart_thread(void *arg)
{
    F4BYScheduler *sched = (F4BYScheduler *)arg;

    while (!sched->_hal_initialized) {
        poll(NULL, 0, 1);
    }
    while (!_f4by_thread_should_exit) {
        sched->delay_microseconds_semaphore(1000);

        // process any pending serial bytes
        ((F4BYUARTDriver *)hal.uartA)->_timer_tick();
        ((F4BYUARTDriver *)hal.uartB)->_timer_tick();
        ((F4BYUARTDriver *)hal.uartC)->_timer_tick();
        ((F4BYUARTDriver *)hal.uartD)->_timer_tick();
        ((F4BYUARTDriver *)hal.uartE)->_timer_tick();
        ((F4BYUARTDriver *)hal.uartF)->_timer_tick();
    }
    return NULL;
}

void *F4BYScheduler::_io_thread(void *arg)
{
    F4BYScheduler *sched = (F4BYScheduler *)arg;

    while (!sched->_hal_initialized) {
        poll(NULL, 0, 1);
    }
    while (!_f4by_thread_should_exit) {
        poll(NULL, 0, 1);

        // run registered IO processes
        perf_begin(sched->_perf_io_timers);
        sched->_run_io();
        perf_end(sched->_perf_io_timers);
    }
    return NULL;
}

void *F4BYScheduler::_storage_thread(void *arg)
{
    F4BYScheduler *sched = (F4BYScheduler *)arg;

    while (!sched->_hal_initialized) {
        poll(NULL, 0, 1);
    }
    while (!_f4by_thread_should_exit) {
        poll(NULL, 0, 10);

        // process any pending storage writes
        perf_begin(sched->_perf_storage_timer);
        ((F4BYStorage *)hal.storage)->_timer_tick();
        perf_end(sched->_perf_storage_timer);
    }
    return NULL;
}

bool F4BYScheduler::in_timerprocess() 
{
    return getpid() != _main_task_pid;
}

void F4BYScheduler::system_initialized() {
    if (_initialized) {
        AP_HAL::panic("PANIC: scheduler::system_initialized called"
                   "more than once");
    }
    _initialized = true;
}

#endif
