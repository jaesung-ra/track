
#include "roi_utils.h"
#include <algorithm>
using namespace std;

bool insidePolygon(ObjPoint p1, const roi& ROI){
    int n = ROI.size();
    if (n < 3) 
        return false;
    ObjPoint extreme = {std::numeric_limits<double>::max(), p1.y};
    int count = 0;
    for (int i=0, j=n-1; i<n; j=i++){
        if (intersect(p1, extreme, ROI[i], ROI[j]))
            count++;
    }
    return count % 2 == 1; // odd->inside, even->not inside
}

bool onSegment(ObjPoint p, ObjPoint q, ObjPoint r)
{
    if (q.x <= max(p.x, r.x) && q.x >= min(p.x, r.x) &&
        q.y <= max(p.y, r.y) && q.y >= min(p.y, r.y))
        return true;
    return false;
}

// To find orientation of ordered triplet (p, q, r).
// The function returns following values
// 0 --> p, q and r are collinear
// 1 --> Clockwise
// 2 --> Counterclockwise
int orientation(ObjPoint p, ObjPoint q, ObjPoint r)
{
    int val = (q.y - p.y) * (r.x - q.x) -
                (q.x - p.x) * (r.y - q.y);

    if (val == 0)
        return 0;             // collinear
    return (val > 0) ? 1 : 2; // clock or counterclock wise
}

// The function that returns true if line segment 'p1q1'
// and 'p2q2' intersect.
bool intersect(ObjPoint p1, ObjPoint q1, ObjPoint p2, ObjPoint q2)
{
    // Find the four orientations needed for general and
    // special cases
    int o1 = orientation(p1, q1, p2);
    int o2 = orientation(p1, q1, q2);
    int o3 = orientation(p2, q2, p1);
    int o4 = orientation(p2, q2, q1);

    // General case
    if (o1 != o2 && o3 != o4)
        return true;

    // Special Cases
    // p1, q1 and p2 are collinear and p2 lies on segment p1q1
    if (o1 == 0 && onSegment(p1, p2, q1))
        return true;

    // p1, q1 and p2 are collinear and q2 lies on segment p1q1
    if (o2 == 0 && onSegment(p1, q2, q1))
        return true;

    // p2, q2 and p1 are collinear and p1 lies on segment p2q2
    if (o3 == 0 && onSegment(p2, p1, q2))
        return true;

    // p2, q2 and q1 are collinear and q1 lies on segment p2q2
    if (o4 == 0 && onSegment(p2, q1, q2))
        return true;

    return false; // Doesn't fall in any of the above cases
}

ObjPoint getIntersectPoint(ObjPoint p1, ObjPoint p2, ObjPoint sp1, ObjPoint sp2) {
    if (p1.x == p2.x)
        return {-1, -1};

    double a1 = (p2.y - p1.y) / (p2.x - p1.x);
    double b1 = (p2.x * p1.y - p1.x * p2.y) / (p2.x - p1.x);

    double a2 = (sp1.y - sp2.y) / (sp1.x - sp2.x);
    double b2 = (sp1.x * sp2.y - sp2.x * sp1.y) / (sp1.x - sp2.x);

    double intersection_x = (b2 - b1) / (a1 - a2);
    ObjPoint p = {intersection_x, a2 * intersection_x + b2};
    return p;
}