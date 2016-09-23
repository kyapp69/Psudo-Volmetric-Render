//
// Created by matt on 5/28/16.
//

#include <stdlib.h>
#include <string.h>
#include "Simulation.h"
#include "log.h"
extern "C"
{
#include "matrix.h"
}

Simulation::Simulation() {
    LOGI("Simulation()");
    frame=0;
    paused=false;
    for (int i = 0; i < MAX_BOXES*3; i++)
        colours[i] = (float) rand() / (float) (RAND_MAX);
    float identityMatrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(int i=0; i<MAX_BOXES; i++)
        memcpy(transforms+(i*16), identityMatrix, sizeof(float)*16);
    for (int i = 2; i < MAX_BOXES; i++) {
        transforms[i * 16 + 12] = 200;
        transforms[i * 16 + 13] = 200;
        transforms[i * 16 + 14] = (float)rand()/(float)(RAND_MAX) * 20.0f - 40.0f;
    }
}

void Simulation::step() {
    if (paused)
        return;
    frame++;
    for(int i=2; i<MAX_BOXES; i++)
    {
        if (transforms[i * 16 + 12] < -30 || transforms[i * 16 + 12] > 30 || transforms[i * 16 + 13] < -20 || transforms[i * 16 + 13] > 20)
        {
            transforms[i*16+12]=(float)rand()/(float)(RAND_MAX) * 40.0f - 20.0f;
            transforms[i*16+13]=(float)rand()/(float)(RAND_MAX) * 30.0f - 15.0f;
            velocities[i*2]=(float)rand()/(float)(RAND_MAX) / 8.0f -(1.0/16.0f);
            velocities[i*2+1]=(float)rand()/(float)(RAND_MAX) / 8.0f -(1.0f/16.0f);
//            LOGI("Reset cube %d (%f, %f, %f)", i, transforms[i*16+12], transforms[i*16+13], transforms[i*16+14]);
        }
        transforms[i*16+12]+=velocities[i*2];
        transforms[i*16+13]+=velocities[i*2+1];
    }
    float rotation[16];
    float temp[16];
    rotate_matrix(frame, 0, 1, 0, rotation);
    identity_matrix(temp);
    translate_matrix(0,0,-28, temp);
    multiply_matrix(temp, rotation, temp);
    memcpy(transforms, temp, sizeof(temp));
    scale_matrix(4,4,4, transforms);
    multiply_matrix(temp, transforms, transforms);

    scale_matrix(4,4,4, transforms+16);
    multiply_matrix(temp, transforms+16, transforms+16);
}

void Simulation::write(uint8_t *buffer, int offset) {
    for (int i=0; i<MAX_BOXES; i++)
    {
        uint8_t *matrix = buffer + offset*i;
        memcpy(matrix, &transforms[i*16], sizeof(float)*16);
    }
}
