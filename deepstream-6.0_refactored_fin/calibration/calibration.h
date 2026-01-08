#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <cmath>
#include <limits>
#include <vector>

extern int POINT[32][4][2];
extern double DISTANCE[32];
extern double VDISTANCE[32];
extern double vanishing_point[32][2][2];
extern double principal_point[32][2];
extern double roadplane[32][4]; 
extern double focal[32];
extern double scale[32];

extern double frameWidth[32];
extern double frameHeight[32];

int max(int a, int b);
int min(int a, int b);

double norm(std::vector<double> a);
double dot(std::vector<double> a, std::vector<double> b);

std::vector<double> matrixSubtraction(std::vector<double> a, std::vector<double> b);
std::vector<double> cross(std::vector<double> a, std::vector<double> b);
std::vector<double> projector(int index, double x, double y);
double getFocal(int index);
std::vector<double> getRoadPlane(int index);
double getSlope(int index, int point1, int point2);
double getIntercept(int index, int point1, int point2);
void calculateVanishingPoint(int index);
void computeCameraCalibration(int index);
double calculateSpeed(double stx, double sty, double edx, double edy, int seconds);
std::vector<double> normalised(const std::vector<double>& v);

#endif