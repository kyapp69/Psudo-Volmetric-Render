#include <math.h>
#include <string.h>

#ifndef MATRIX_H
#define MATRIX_H

/*
 * Simulates desktop's glRotatef. The matrix is returned in column-major
 * order.
 */
void rotate_matrix(double angle, double x, double y, double z, float *R);

/*
 * Simulates gluPerspectiveMatrix
 */
void perspective_matrix(double fovy, double aspect, double znear, double zfar, float *P);
/*
 * Multiplies A by B and writes out to C. All matrices are 4x4 and column
 * major. In-place multiplication is supported.
 */
void multiply_matrix(float *A, float *B, float *C);

void identity_matrix(float *matrix);

void translate_matrix(double x, double y, double z, float *matrix);

void scale_matrix(double x, double y, double z, float *matrix);


#endif
