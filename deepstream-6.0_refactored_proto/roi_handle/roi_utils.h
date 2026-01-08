#ifndef ROI_UTILS_H
#define ROI_UTILS_H

#include <vector>
#include <limits>
#include "process_meta.h"

using roi = std::vector<ObjPoint>;

bool insidePolygon(ObjPoint p1, const roi& ROI);
bool onSegment(ObjPoint p, ObjPoint q, ObjPoint r);
int orientation(ObjPoint p, ObjPoint q, ObjPoint r);
bool intersect(ObjPoint p1, ObjPoint q1, ObjPoint p2, ObjPoint q2);
ObjPoint getIntersectPoint(ObjPoint p1, ObjPoint p2, ObjPoint sp1, ObjPoint sp2);

#endif 
