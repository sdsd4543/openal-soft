/**
 * OpenAL cross platform audio library
 * Copyright (C) 2011 by Chris Robinson.
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA  02111-1307, USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>

#include "alMain.h"
#include "alFilter.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "alu.h"


typedef struct ALdedicatedState {
    DERIVE_FROM_TYPE(ALeffectState);

    ALfloat gains[MaxChannels];
} ALdedicatedState;


static ALvoid DedicatedDestroy(ALeffectState *effect)
{
    ALdedicatedState *state = GET_PARENT_TYPE(ALdedicatedState, ALeffectState, effect);
    free(state);
}

static ALboolean DedicatedDeviceUpdate(ALeffectState *effect, ALCdevice *Device)
{
    (void)effect;
    (void)Device;
    return AL_TRUE;
}

static ALvoid DedicatedUpdate(ALeffectState *effect, ALCdevice *device, const ALeffectslot *Slot)
{
    ALdedicatedState *state = GET_PARENT_TYPE(ALdedicatedState, ALeffectState, effect);
    ALfloat Gain;
    ALsizei s;

    Gain = Slot->Gain * Slot->effect.Dedicated.Gain;
    for(s = 0;s < MaxChannels;s++)
        state->gains[s] = 0.0f;

    if(Slot->effect.type == AL_EFFECT_DEDICATED_DIALOGUE)
        ComputeAngleGains(device, atan2f(0.0f, 1.0f), 0.0f, Gain, state->gains);
    else if(Slot->effect.type == AL_EFFECT_DEDICATED_LOW_FREQUENCY_EFFECT)
        state->gains[LFE] = Gain;
}

static ALvoid DedicatedProcess(ALeffectState *effect, ALuint SamplesToDo, const ALfloat *RESTRICT SamplesIn, ALfloat (*RESTRICT SamplesOut)[BUFFERSIZE])
{
    ALdedicatedState *state = GET_PARENT_TYPE(ALdedicatedState, ALeffectState, effect);
    const ALfloat *gains = state->gains;
    ALuint i, c;

    for(c = 0;c < MaxChannels;c++)
    {
        if(!(gains[c] > 0.00001f))
            continue;

        for(i = 0;i < SamplesToDo;i++)
            SamplesOut[c][i] = SamplesIn[i] * gains[c];
    }
}

ALeffectState *DedicatedCreate(void)
{
    ALdedicatedState *state;
    ALsizei s;

    state = malloc(sizeof(*state));
    if(!state)
        return NULL;

    GET_DERIVED_TYPE(ALeffectState, state)->Destroy = DedicatedDestroy;
    GET_DERIVED_TYPE(ALeffectState, state)->DeviceUpdate = DedicatedDeviceUpdate;
    GET_DERIVED_TYPE(ALeffectState, state)->Update = DedicatedUpdate;
    GET_DERIVED_TYPE(ALeffectState, state)->Process = DedicatedProcess;

    for(s = 0;s < MaxChannels;s++)
        state->gains[s] = 0.0f;

    return GET_DERIVED_TYPE(ALeffectState, state);
}

void ded_SetParami(ALeffect *effect, ALCcontext *context, ALenum param, ALint val)
{ (void)effect;(void)param;(void)val; alSetError(context, AL_INVALID_ENUM); }
void ded_SetParamiv(ALeffect *effect, ALCcontext *context, ALenum param, const ALint *vals)
{
    ded_SetParami(effect, context, param, vals[0]);
}
void ded_SetParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat val)
{
    switch(param)
    {
        case AL_DEDICATED_GAIN:
            if(val >= 0.0f && isfinite(val))
                effect->Dedicated.Gain = val;
            else
                alSetError(context, AL_INVALID_VALUE);
            break;

        default:
            alSetError(context, AL_INVALID_ENUM);
            break;
    }
}
void ded_SetParamfv(ALeffect *effect, ALCcontext *context, ALenum param, const ALfloat *vals)
{
    ded_SetParamf(effect, context, param, vals[0]);
}

void ded_GetParami(ALeffect *effect, ALCcontext *context, ALenum param, ALint *val)
{ (void)effect;(void)param;(void)val; alSetError(context, AL_INVALID_ENUM); }
void ded_GetParamiv(ALeffect *effect, ALCcontext *context, ALenum param, ALint *vals)
{
    ded_GetParami(effect, context, param, vals);
}
void ded_GetParamf(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *val)
{
    switch(param)
    {
        case AL_DEDICATED_GAIN:
            *val = effect->Dedicated.Gain;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM);
            break;
    }
}
void ded_GetParamfv(ALeffect *effect, ALCcontext *context, ALenum param, ALfloat *vals)
{
    ded_GetParamf(effect, context, param, vals);
}
