/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl) with additions from Sungeun K. Jeon (https://github.com/chamnit/grbl)
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Stepper.h"

#include "libs/Module.h"
#include "libs/Kernel.h"
#include "Planner.h"
#include "Conveyor.h"
#include "StepperMotor.h"

#include <vector>
using namespace std;

#include "libs/nuts_bolts.h"
#include "libs/Hook.h"

#include <mri.h>


// The stepper reacts to blocks that have XYZ movement to transform them into actual stepper motor moves
// TODO: This does accel, accel should be in StepperMotor

Stepper::Stepper(){
    current_block = NULL;
    paused = false;
    force_speed_update = false;
}

//Called when the module has just been loaded
void Stepper::on_module_loaded()
{
    register_for_event(ON_CONFIG_RELOAD);
    register_for_event(ON_BLOCK_BEGIN);
    register_for_event(ON_BLOCK_END);
    register_for_event(ON_GCODE_EXECUTE);
    register_for_event(ON_GCODE_RECEIVED);
    register_for_event(ON_PLAY);
    register_for_event(ON_PAUSE);

    // Get onfiguration
    on_config_reload(this);

    // Acceleration ticker
    acceleration_tick_hook = THEKERNEL->slow_ticker->attach( acceleration_ticks_per_second, this, &Stepper::trapezoid_generator_tick );

    // Attach to the end_of_move stepper event
    for (auto a : THEKERNEL->robot->actuators)
        a->attach(this, &Stepper::stepper_motor_finished_move);
}

// Get configuration from the config file
void Stepper::on_config_reload(void* argument)
{
    acceleration_ticks_per_second =  THEKERNEL->config->value(acceleration_ticks_per_second_checksum)->by_default(100   )->as_number();
    minimum_steps_per_second      =  THEKERNEL->config->value(minimum_steps_per_minute_checksum     )->by_default(3000  )->as_number() / 60.0F;

    // Steppers start off by default
    turn_enable_pins_off();
}

// When the play/pause button is set to pause, or a module calls the ON_PAUSE event
void Stepper::on_pause(void* argument)
{
    paused = true;
    for (auto a : THEKERNEL->robot->actuators)
        a->pause();
}

// When the play/pause button is set to play, or a module calls the ON_PLAY event
void Stepper::on_play(void* argument)
{
    // TODO: Re-compute the whole queue for a cold-start
    paused = false;
    for (auto a : THEKERNEL->robot->actuators)
        a->unpause();
}

void Stepper::on_gcode_received(void* argument)
{
    Gcode* gcode = static_cast<Gcode*>(argument);
    // Attach gcodes to the last block for on_gcode_execute
    if (gcode->has_m && (gcode->m == 84 || gcode->m == 17 || gcode->m == 18 ))
        THEKERNEL->conveyor->append_gcode(gcode);
}

// React to enable/disable gcodes
void Stepper::on_gcode_execute(void* argument)
{
    Gcode* gcode = static_cast<Gcode*>(argument);

    if( gcode->has_m)
    {
        if (gcode->m == 17)
            turn_enable_pins_on();
        else if ((gcode->m == 84 || gcode->m == 18) && !gcode->has_letter('E'))
            turn_enable_pins_off();
    }
}

// Enable steppers
void Stepper::turn_enable_pins_on()
{
    for (auto a : THEKERNEL->robot->actuators)
        a->enable(true);
    enable_pins_status = true;
}

// Disable steppers
void Stepper::turn_enable_pins_off()
{
    for (auto a : THEKERNEL->robot->actuators)
        a->enable(false);
    enable_pins_status = false;
}

// A new block is popped from the queue
void Stepper::on_block_begin(void* argument)
{
    Block* block  = static_cast<Block*>(argument);

    // The stepper does not care about 0-blocks
    if (block->millimeters == 0.0F)
        return;

    // Mark the new block as of interrest to us
    if (block->steps[ALPHA_STEPPER] > 0 || block->steps[BETA_STEPPER] > 0 || block->steps[GAMMA_STEPPER] > 0)
        block->take();
    else
        return;

    // We can't move with the enable pins off
    if (enable_pins_status == false)
        turn_enable_pins_on();

    current_block = block;

    // Setup acceleration for this block
    trapezoid_generator_reset();

    // Find the stepper with the more steps, it's the one the speed calculations will want to follow
    main_stepper = NULL;
    for (auto a : THEKERNEL->robot->actuators)
        if ((main_stepper == NULL) || (a->steps_to_move > main_stepper->steps_to_move))
            main_stepper = a;

    // TODO: use auto a : THEKERNEL->robot->actuators, and move block->steps to a per-Actuator ActionData
    for (int i = 0; i < 3; i++)
    {
        if (block->steps[i])
        {
            // Setup : instruct stepper motors to move
            THEKERNEL->robot->actuators[i]->move( (block->direction_bits >> i) & 1, block->steps[i]);

            // set rate ratio for each actuator - ie how fast each actuator moves compared to the movement of the head in cartesian space
            THEKERNEL->robot->actuators[i]->rate_ratio = ( (float) block->steps[i] / (float) block->steps_event_count );
        }
    }

    // Set the initial speed for this move
    trapezoid_generator_tick(0);

    // Synchronise the acceleration curve with the stepping
    synchronize_acceleration(0);
}

// Current block is discarded
void Stepper::on_block_end(void* argument)
{
    current_block = NULL;
}

// When a stepper motor has finished it's assigned movement
uint32_t Stepper::stepper_motor_finished_move(uint32_t dummy)
{
    // We care only if none is still moving
    for (auto a : THEKERNEL->robot->actuators)
        if (a->moving)
            return 0;

    // This block is finished, release it
    if (current_block)
        current_block->release();

    return 0;
}


// This is called ACCELERATION_TICKS_PER_SECOND times per second by the step_event
// interrupt. It can be assumed that the trapezoid-generator-parameters and the
// current_block stays untouched by outside handlers for the duration of this function call.
uint32_t Stepper::trapezoid_generator_tick( uint32_t dummy )
{
    // Do not do the accel math for nothing
    if (current_block && !paused && THEKERNEL->step_ticker->active_motor_bm)
    {
        // Store this here because we use it a lot down there
        uint32_t current_steps_completed = main_stepper->stepped;

        // Do not accel, just set the value
        if (force_speed_update)
        {
            force_speed_update = false;
        }
        // if we are flushing the queue, decelerate to 0 then finish this block
        else if (THEKERNEL->conveyor->flush)
        {
            if (trapezoid_adjusted_rate > current_block->rate_delta * 1.5F)
            {
                trapezoid_adjusted_rate -= current_block->rate_delta;
            }
            else if (trapezoid_adjusted_rate == current_block->rate_delta * 0.5F)
            {
                for (auto i : THEKERNEL->robot->actuators)
                    i->move(i->direction, 0);

                if (current_block)
                    current_block->release();

                return 0;
            }
            else
            {
                trapezoid_adjusted_rate = current_block->rate_delta * 0.5F;
            }
        }
        // If we are accelerating
        else if (current_steps_completed <= current_block->accelerate_until + 1)
        {
            // Increase speed
            trapezoid_adjusted_rate += current_block->rate_delta;
            if (trapezoid_adjusted_rate > current_block->nominal_rate)
                trapezoid_adjusted_rate = current_block->nominal_rate;
        }
        // If we are decelerating
        else if (current_steps_completed > current_block->decelerate_after)
        {
            // Reduce speed
            // NOTE: We will only reduce speed if the result will be > 0. This catches small
            // rounding errors that might leave steps hanging after the last trapezoid tick.
            if (trapezoid_adjusted_rate > current_block->rate_delta * 1.5F)
                trapezoid_adjusted_rate -= current_block->rate_delta;
            else
                trapezoid_adjusted_rate = current_block->rate_delta * 0.5F;

            if (trapezoid_adjusted_rate < current_block->final_rate )
                trapezoid_adjusted_rate = current_block->final_rate;
        }
        // If we are cruising
        else if (trapezoid_adjusted_rate != current_block->nominal_rate)
        {
            trapezoid_adjusted_rate = current_block->nominal_rate;
        }

        set_step_events_per_second(trapezoid_adjusted_rate);
    }

    return 0;
}



// Initializes the trapezoid generator from the current block. Called whenever a new
// block begins.
inline void Stepper::trapezoid_generator_reset()
{
    trapezoid_adjusted_rate = current_block->initial_rate;
    force_speed_update = true;
}

// Update the speed for all steppers
void Stepper::set_step_events_per_second( float steps_per_second )
{
    // We do not step slower than this
    //steps_per_second = max(steps_per_second, minimum_steps_per_second);
    if (steps_per_second < minimum_steps_per_second)
        steps_per_second = minimum_steps_per_second;

    // Instruct the stepper motors
    for (auto a : THEKERNEL->robot->actuators)
        if (a->moving)
            a->set_speed(steps_per_second * a->rate_ratio);

    // Other modules might want to know the speed changed
    THEKERNEL->call_event(ON_SPEED_CHANGE, this);
}

// This function has the role of making sure acceleration and deceleration curves have their
// rhythm synchronized. The accel/decel must start at the same moment as the speed update routine
// This is caller in "step just occured" or "block just began" ( step Timer ) context, so we need to be fast.
// All we do is reset the other timer so that it does what we want
uint32_t Stepper::synchronize_acceleration(uint32_t dummy)
{
    // No move was done, this is called from on_block_begin
    // This means we setup the accel timer in a way where it gets called right after
    // we exit this step interrupt, and so that it is then in synch with
    if (main_stepper->stepped == 0)
    {
        // Whatever happens, we must call the accel interrupt asap
        // Because it will set the initial rate
        // We also want to synchronize in case we start accelerating or decelerating now

        // Accel interrupt must happen asap
        NVIC_SetPendingIRQ(TIMER2_IRQn);
        // Synchronize both counters
        LPC_TIM2->TC = LPC_TIM0->TC;

        // If we start decelerating after this, we must ask the actuator to warn us
        // so we can do what we do in the "else" bellow
        if (current_block->decelerate_after > 0 && current_block->decelerate_after < main_stepper->steps_to_move)
            main_stepper->attach_signal_step(current_block->decelerate_after, this, &Stepper::synchronize_acceleration);
    }
    else
    {
        // If we are called not at the first steps, this means we are beginning deceleration
        NVIC_SetPendingIRQ(TIMER2_IRQn);
        // Synchronize both counters
        LPC_TIM2->TC = LPC_TIM0->TC;
    }

    return 0;
}

