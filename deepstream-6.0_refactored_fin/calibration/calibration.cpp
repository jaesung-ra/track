#include <climits>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include "calibration.h"
#include "../../common/common_types.h"

using namespace std;

int POINT[32][4][2] = {
    {{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}},
};

double DISTANCE[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1
};

double VDISTANCE[32] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1
};

double scale_longitude[32] = {-1, };
double scale_latitude[32] = {-1, };
double u_longitude[32][3];
double u_latitude[32][3];

double vanishing_point[32][2][2] = {
    {{-1, -1}, {-1, -1}},
};

double principal_point[32][2] = {
    {-1, -1},
};

double roadplane[32][4] = {
    {-1, -1, -1, -1},
};

double focal[32] = {-1,};
double scale[32] = {-1,};
double frameWidth[32] = {-1,};
double frameHeight[32] = {-1,};

int max(int a, int b) {
    return (a > b) ? a : b;
}

int min(int a, int b) {
    return (a < b) ? a : b;
}

double norm(std::vector<double> a) {
    double sum = 0;
    for (unsigned int i = 0; i < a.size(); i++)
    sum += a[i] * a[i];
return sqrt(sum);
}

std::vector<double> normalised(const std::vector<double>& v) {
    double n = norm(v);
    return {v[0]/n, v[1]/n, v[2]/n};
}

double dot(std::vector<double> a, std::vector<double> b) {
    double sum = 0;
    for (unsigned int i = 0; i < a.size(); i++)
        sum += a[i] * b[i];
    return sum;
}

std::vector<double> matrixSubtraction(std::vector<double> a, std::vector<double> b) {
    std::vector<double> subtract;
    for (unsigned int i = 0; i < a.size(); i++)
        subtract.push_back(a[i] - b[i]);
    return subtract;
}

std::vector<double> cross(std::vector<double> a, std::vector<double> b) {
    std::vector<double> c;
    c.push_back(a[1] * b[2] - a[2] * b[1]);
    c.push_back(a[2] * b[0] - a[0] * b[2]);
    c.push_back(a[0] * b[1] - a[1] * b[0]);
    return c;
}

std::vector<double> projector(int index, double x, double y) {
    std::vector<double> ppW = {principal_point[index][0], principal_point[index][1], 0, 1};
    std::vector<double> pW = {x, y, focal[index]};
    std::vector<double> dirVec = matrixSubtraction(pW, ppW);
    std::vector<double> road = {roadplane[index][0], roadplane[index][1], roadplane[index][2], roadplane[index][3]};

    double t = -1 * dot(road, ppW);
    road.pop_back();
    t /= dot(road, dirVec);

    std::vector<double> proj;
    for (int i = 0; i < 3; i++)
        proj.push_back(ppW[i] + t * dirVec[i]);

    return proj;
}

double getFocal(int index) {
    std::vector<double> t1 = {
        vanishing_point[index][0][0] - principal_point[index][0],
        vanishing_point[index][0][1] - principal_point[index][1]
    };
    std::vector<double> t2 = {
        vanishing_point[index][1][0] - principal_point[index][0],
        vanishing_point[index][1][1] - principal_point[index][1]
    };
    return sqrt(abs(-1 * dot(t1, t2)));
}

std::vector<double> getRoadPlane(int index) {
    std::vector<double> U = {vanishing_point[index][0][0], vanishing_point[index][0][1], focal[index]};
    std::vector<double> V = {vanishing_point[index][1][0], vanishing_point[index][1][1], focal[index]};
    std::vector<double> C = {principal_point[index][0], principal_point[index][1], 0};

    std::vector<double> W = cross(matrixSubtraction(U, C), matrixSubtraction(V, C));

    std::vector<double> w = {
        W[0] / W[2] * focal[index] + C[0],
        W[1] / W[2] * focal[index] + C[1], 1};

    std::vector<double> p = {w[0] - C[0], w[1] - C[1], focal[index] - C[2]};
    double pNorm = norm(p);

    std::vector<double> roadPlane = {p[0] / pNorm, p[1] / pNorm, p[2] / pNorm, 10};
    return roadPlane;
}

double getSlope(int index, int point1, int point2) {
    return (POINT[index][point1][1] - POINT[index][point2][1]) /
           (double)(POINT[index][point1][0] - POINT[index][point2][0]);
}

double getIntercept(int index, int point1, int point2) {
    return (POINT[index][point1][0] * POINT[index][point2][1] -
            POINT[index][point2][0] * POINT[index][point1][1]) /
           (double)(POINT[index][point1][0] - POINT[index][point2][0]);
}

void calculateVanishingPoint(int index) {
    double a1 = getSlope(index, 0, 1);
    double b1 = getIntercept(index, 0, 1);

    double a2 = getSlope(index, 2, 3);
    double b2 = getIntercept(index, 2, 3);

    double intersection_x = POINT[index][0][0] - POINT[index][1][0] == 0
        ? POINT[index][0][0]
        : (POINT[index][2][0] - POINT[index][3][0] == 0
            ? POINT[index][2][0]
            : (b2 - b1) / (a1 - a2));

    vanishing_point[index][0][0] = intersection_x;
    vanishing_point[index][0][1] = POINT[index][0][0] - POINT[index][1][0] == 0
        ? a2 * intersection_x + b2
        : a1 * intersection_x + b1;

    vanishing_point[index][1][0] = INT_MAX;
    vanishing_point[index][1][1] = vanishing_point[index][0][1];
}

// 속도 계산을 위해 Calibration ROI 좌표를 사용하여 소실점, 초점, 도로 평면 계산 후 전역 변수에 저장
void computeCameraCalibration(int index) {
    calculateVanishingPoint(index);

    principal_point[index][0] = frameWidth[index] / 2;
    principal_point[index][1] = frameHeight[index] / 2;

    focal[index] = getFocal(index);

    std::vector<double> rp = getRoadPlane(index);

    roadplane[index][0] = rp[0];
    roadplane[index][1] = rp[1];
    roadplane[index][2] = rp[2];
    roadplane[index][3] = rp[3];

    scale[index] = DISTANCE[index] /
                   norm(matrixSubtraction(projector(index, POINT[index][0][0], POINT[index][0][1]),
                                          projector(index, POINT[index][1][0], POINT[index][1][1])));

    std::vector<double> p0 = projector(index, POINT[index][0][0], POINT[index][0][1]);
    std::vector<double> p1 = projector(index, POINT[index][1][0], POINT[index][1][1]);
    std::vector<double> p2 = projector(index, POINT[index][2][0], POINT[index][2][1]);

    std::vector<double> longitude_vec = matrixSubtraction(p1, p0);
    std::vector<double> latitude_vec  = matrixSubtraction(p2, p1);

    std::vector<double> n_longitude = normalised(longitude_vec);
    std::vector<double> n_latitude  = normalised(latitude_vec);

    for (int i=0; i<3; i++) {
        u_longitude[index][i] = n_longitude[i];
        u_latitude [index][i] = n_latitude [i];
    }

    scale_longitude[index] = DISTANCE [index] / norm(longitude_vec);
    scale_latitude [index] = VDISTANCE[index] / norm(latitude_vec );

    printf("[MSG] " CYN "  Calbiration file info: \n" RESET);
    printf("[MSG] " CYN "  vp1:" RESET " %.2f %.2f\n", vanishing_point[index][0][0], vanishing_point[index][0][1]);
    printf("[MSG] " CYN "  vp2:" RESET " %.2f %.2f\n", vanishing_point[index][1][0], vanishing_point[index][1][1]);
    printf("[MSG] " CYN "  pp:" RESET " %.2f %.2f\n", principal_point[index][0], principal_point[index][1]);
    printf("[MSG] " CYN "  roadPlane:" RESET " %.2f %.2f %.2f %.2f\n",
           roadplane[index][0], roadplane[index][1],
           roadplane[index][2], roadplane[index][3]);
    printf("[MSG] " CYN "  focal:" RESET " %.2f\n", focal[index]);
    printf("[MSG] " CYN "  scale:" RESET " %.8f\n", scale[index]);
    printf("[MSG] " CYN "  longitude scale:" RESET " %.8f\n", scale_longitude[index]);
    printf("[MSG] " CYN "  latitude scale:" RESET " %.8f\n",  scale_latitude [index]);
    printf("\n");
}

double calculateSpeed(double stx, double sty, double edx, double edy, int seconds) {
    int index = 0;
    try {
        std::vector<double> start_point = projector(index, stx, sty);
        std::vector<double> end_point   = projector(index, edx, edy);
        std::vector<double> d = matrixSubtraction(end_point, start_point);

        double d_longitude = dot(d, {u_longitude[index][0], u_longitude[index][1], u_longitude[index][2]});
        double d_latitude  = dot(d, {u_latitude [index][0], u_latitude [index][1], u_latitude [index][2]});
        double meters = std::hypot( d_longitude * scale_longitude[index], d_latitude * scale_latitude[index]);

        return meters * 3.6 / seconds;
    } 
	catch (exception& err) {
        return 0;
    }
}