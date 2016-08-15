
struct Vertex{
    float posX, posY, posZ, posW; // Position data
};

#define XYZ1(_x_, _y_, _z_) (_x_), (_y_), (_z_), 1.f
#define XYZp5(_x_, _y_, _z_) (_x_), (_y_), (_z_), 0.6

static const struct Vertex vertexDataCube[] = {
        XYZ1(-1, -1, -1),
        XYZ1(1, -1, -1),
        XYZ1(-1, 1, -1),
        XYZ1(-1, 1, -1),
        XYZ1(1, -1, -1),
        XYZ1(1, 1, -1),

        XYZ1(-1, -1, 1),
        XYZ1(-1, 1, 1),
        XYZ1(1, -1, 1),
        XYZ1(1, -1, 1),
        XYZ1(-1, 1, 1),
        XYZ1(1, 1, 1),

        XYZ1(1, 1, 1),
        XYZ1(1, 1, -1),
        XYZ1(1, -1, 1),
        XYZ1(1, -1, 1),
        XYZ1(1, 1, -1),
        XYZ1(1, -1, -1),

        XYZ1(-1, 1, 1),
        XYZ1(-1, -1, 1),
        XYZ1(-1, 1, -1),
        XYZ1(-1, 1, -1),
        XYZ1(-1, -1, 1),
        XYZ1(-1, -1, -1),

        XYZ1(1, 1, 1),
        XYZ1(-1, 1, 1),
        XYZ1(1, 1, -1),
        XYZ1(1, 1, -1),
        XYZ1(-1, 1, 1),
        XYZ1(-1, 1, -1),

        XYZ1(1, -1, 1),
        XYZ1(1, -1, -1),
        XYZ1(-1, -1, 1),
        XYZ1(-1, -1, 1),
        XYZ1(1, -1, -1),
        XYZ1(-1, -1, -1),
};

static const struct Vertex vetrexDataPyramid[] = {
        XYZ1(-1, -1, -1),
        XYZ1(1, -1, -1),
        XYZ1(0, 1, 0),

        XYZ1(-1, -1, 1),
        XYZ1(0, 1, 0),
        XYZ1(1, -1, 1),

        XYZ1(1, -1, 1),
        XYZ1(0, 1, 0),
        XYZ1(1, -1, -1),

        XYZ1(0, 1, 0),
        XYZ1(-1, -1, 1),
        XYZ1(-1, -1, -1),

        XYZ1(1, -1, 1),
        XYZ1(1, -1, -1),
        XYZ1(-1, -1, 1),
        XYZ1(-1, -1, 1),
        XYZ1(1, -1, -1),
        XYZ1(-1, -1, -1),
};
