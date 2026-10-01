#pragma once
#include "core.hpp"

namespace sn {
struct Vec { double x=0,y=0,z=0; };
inline Vec operator+(Vec a,Vec b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec operator-(Vec a,Vec b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec operator*(Vec a,double s){return {a.x*s,a.y*s,a.z*s};}
inline double dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline Vec cross(Vec a,Vec b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline double length(Vec a){return std::sqrt(dot(a,a));}
inline Vec rotate(Vec v,Vec axis,double angle){double n=length(axis);if(n<1e-12)return v;axis=axis*(1/n);return v*std::cos(angle)+cross(axis,v)*std::sin(angle)+axis*(dot(axis,v)*(1-std::cos(angle)));}
struct Camera {
    Vec right{1,0,0},up{0,1,0},back{0,0,1},position{0,0,10};
};
inline void navigate(Camera& c,Vec pivot,const std::array<int16_t,6>& a,double dt,double scale,bool orbit,bool rotatable,bool perspective) {
    dt=std::clamp(dt,0.0,0.05);scale=std::clamp(scale,1e-6,1e12);
    Vec pan=c.right*(a[0]/350.0)+c.up*(-a[2]/350.0);
    if(perspective)pan=pan+c.back*(a[1]/350.0);
    c.position=c.position+pan*(scale*dt);
    if(rotatable) {
        Vec axis=c.right*(-a[3]/350.0)+c.up*(a[5]/350.0)+c.back*(-a[4]/350.0);
        double angle=length(axis)*dt*1.8;
        c.right=rotate(c.right,axis,angle);c.up=rotate(c.up,axis,angle);c.back=rotate(c.back,axis,angle);
        if(orbit)c.position=pivot+rotate(c.position-pivot,axis,angle);
    }
}
// Transport-independent view step. Native callbacks and asynchronous web
// callbacks populate the same snapshot, then publish its resulting properties.
struct NavigationView {
    Camera camera;
    Vec pivot;
    bool perspective=true,rotatable=true,orbit=true,hasExtents=false;
    std::array<double,6> extents{};
    void advance(const std::array<int16_t,6>& axes,double dt,double scale){
        navigate(camera,pivot,axes,dt,scale,orbit,rotatable,perspective);
        if(!perspective&&hasExtents&&axes[1]){
            double factor=std::exp(std::clamp(axes[1]/350.0*std::clamp(dt,0.0,0.05)*2.0,-1.0,1.0));
            for(int i=0;i<2;++i){double centre=extents[i]/2+extents[i+3]/2;extents[i]=centre+(extents[i]-centre)*factor;extents[i+3]=centre+(extents[i+3]-centre)*factor;}
        }
    }
};
}
