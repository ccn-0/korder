#include <Windows.h>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <iostream>
#include <iomanip>
#include <vector>
#include "interception.h"

#define RECORD_RESERVE 1024

class context {
public:
    context();
    ~context();

    inline operator InterceptionContext() const noexcept { return context_; }

private:
    InterceptionContext context_;
};

context::context() {
    context_ = interception_create_context();
    if (!context_) {
        throw std::runtime_error("failed to create interception context");
    }
}

context::~context() {
    interception_destroy_context(context_);
}

enum ScanCode
{
    SCANCODE_F1 = 0x3B,
    SCANCODE_F2 = 0x3C,
    SCANCODE_ESC = 0x01,
    SCANCODE_A = 0x1E,
    SCANCODE_D = 0x20,
    SCANCODE_W = 0x11,
    SCANCODE_SPACE = 0x39,
    SCANCODE_LEFT = 0x4B,
    SCANCODE_RIGHT = 0x4D
};

enum AppState
{
    APP_IDLE = 0x00,
    APP_RECORDING = 0x01,
    APP_REPLAYING = 0x02
};


struct entry {
    InterceptionDevice device;
    InterceptionStroke stroke;
    std::chrono::steady_clock::time_point timestamp;

    entry(InterceptionDevice device, InterceptionStroke ims, std::chrono::steady_clock::time_point timestamp)
        : device(device), timestamp(timestamp)
    {
        for (int i = 0; i < sizeof(InterceptionStroke); i++) {
            stroke[i] = ims[i];
        }
    }
};


// Vector to store all recorded inputs in sequence
std::vector<entry> record;
unsigned int record_ptr = 0;

AppState state = APP_IDLE;
// Reference timepoint to replay starting
std::chrono::steady_clock::time_point t_replay_start;


context ctx;

void wait_for_next(entry &current)
{
    /* This is busy wait implementation, thread_sleep was not reliable. Rewrite this
     * if you find good enough sleep implementation for this (needs sub 1ms precision) */
    if (record_ptr < record.size())
    {
        entry& next = record[record_ptr];
        auto expected_wait = std::chrono::duration<double, std::micro>(next.timestamp - current.timestamp).count();
        auto t_reference = std::chrono::high_resolution_clock::now();
        while (true)
        {
            auto t_now = std::chrono::high_resolution_clock::now();
            auto dur = std::chrono::duration<double, std::micro>(t_now - t_reference).count();
            if (expected_wait < dur)
            {
                // Enough time has passed, exit the busy wait
                break;
            }
        }
    }
}

void thread_replay()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (true)
    {
        if (state == APP_REPLAYING && record_ptr < record.size())
        {     
            entry& current = record[record_ptr];
            interception_send(ctx, current.device, &(current.stroke), 1);
            record_ptr++;
            wait_for_next(current); // Busy wait so the next input can be sent
        }
    }
}

bool handle_intercept(InterceptionDevice device, InterceptionStroke &stroke)
{
    auto ts = std::chrono::high_resolution_clock::now();

    // Meta stroke (F1 or F2 button) to toggle active state of the recorder
    if (interception_is_keyboard(device))
    {
        InterceptionKeyStroke& ks = *(InterceptionKeyStroke*)&stroke;
        if (ks.code == SCANCODE_F1 && ks.state == 0)
        {
            switch (state)
            {
            case APP_IDLE:
                // Started recording, clear the recorded inputs
                record.clear();
                state = APP_RECORDING;
                std::cout << "[Info] Recording started - previously recorded queue was cleared\n";
                break;
            case APP_RECORDING:
                state = APP_IDLE;
                std::cout << "[Info] Recording stopped\n";
                std::cout << "       Inputs in queue: " << record.size() << "\n";
                break;
            case APP_REPLAYING:
                std::cout << "[Errr] You must stop the replay before you can start recording!\n";
                break;
            default:
                break;
            }
            return false;
        }
        else if (ks.code == SCANCODE_F2 && ks.state == 0)
        {
            switch (state)
            {
            case APP_IDLE:
                record_ptr = 0;
                t_replay_start = std::chrono::high_resolution_clock::now();
                state = APP_REPLAYING;
                std::cout << "[Info] Replay started\n";         
                break;
            case APP_RECORDING:
                std::cout << "[Errr] You must stop recording before you can start replaying!\n";
                break;
            case APP_REPLAYING:
                state = APP_IDLE;
                std::cout << "[Info] Replay stopped\n";
                break;
            default:
                break;
            }
            return false;
        }
    }

    if (state == APP_RECORDING)
    {
        entry es(device, stroke, ts);
        record.push_back(es);
    }

    if (state == APP_REPLAYING)
    {
        // Tell main loop to not forward any intercepted inputs
        return false;
    }
    else 
    {
        return true;
    }

    
}


int main() {

    std::cout << "korder init... \n";

    std::thread(thread_replay).detach();

    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

    InterceptionDevice device; // device 1 is keyboard, device 11 is mouse
    InterceptionStroke stroke;
    std::chrono::steady_clock::time_point ts; // Last timestamp of any intercepted event

    interception_set_filter(ctx, interception_is_mouse, INTERCEPTION_FILTER_MOUSE_ALL);
    interception_set_filter(ctx, interception_is_keyboard, INTERCEPTION_FILTER_KEY_ALL);

    record.reserve(RECORD_RESERVE);

    while (interception_receive(ctx, device = interception_wait(ctx), &stroke, 1) > 0)
    {  
        if( handle_intercept(device, stroke) )
            interception_send(ctx, device, &stroke, 1);
    }

    interception_destroy_context(ctx);

    return 0;
}