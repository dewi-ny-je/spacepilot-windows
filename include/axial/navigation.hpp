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
// Standard views, as the 3Dconnexion driver's predefined view commands.
enum class View : uint8_t {front=1,back,left,right,top,bottom,iso1,iso2};
constexpr uint32_t viewCommand(View v){return commandView|(uint32_t(v)<<24);}
inline bool commandedView(uint32_t flags,View& v){
    unsigned n=flags>>24;if(!(flags&commandView)||n<1||n>8)return false;v=View(n);return true;
}
inline Vec normalized(Vec v){double n=length(v);return n>1e-12?v*(1/n):Vec{};}
// The front camera's axes in client coordinates. coordinateSystem is the
// column-major transform from client to navlib coordinates (Y up, Z out of the
// screen); the navlib's own axes are its inverse rotation, the transposed rows.
inline bool frontFromCoordinateSystem(const double m[16],Camera& front){
    Camera c;c.right=normalized({m[0],m[4],m[8]});c.up=normalized({m[1],m[5],m[9]});c.back=normalized({m[2],m[6],m[10]});
    if(length(c.right)==0||length(c.up)==0||length(c.back)==0||std::abs(dot(c.right,c.up))>1e-6||std::abs(dot(cross(c.right,c.up),c.back)-1)>1e-6)return false;
    front.right=c.right;front.up=c.up;front.back=c.back;return true;
}
// Orients the camera for a standard view relative to the front view; the
// position is left for the caller to fit.
inline Camera orient(View v,const Camera& front){
    Vec r=front.right,u=front.up,b=front.back;Camera c=front;
    auto set=[&](Vec back,Vec up){c.back=normalized(back);c.up=normalized(up-c.back*dot(up,c.back));c.right=cross(c.up,c.back);};
    switch(v){
        case View::front:set(b,u);break;
        case View::back:set(b*-1,u);break;
        case View::left:set(r*-1,u);break;
        case View::right:set(r,u);break;
        case View::top:set(u,b*-1);break;
        case View::bottom:set(u*-1,b);break;
        case View::iso1:set(r+u+b,u);break;      // front, right, top
        case View::iso2:set(r*-1+u+b,u);break;   // front, left, top
    }
    return c;
}
}
