// 真实 11 维 auto_aim 模型下，同时驱动"新版 EKF(先验 NIS)"与"旧版 EKF(后验 NIS)"，
// 逐帧比较状态 x 与协方差 P，并统计两者的 NIS 分布与 Bad-Converge 触发行为。
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "old_ekf.hpp"
#include "tools/extended_kalman_filter.hpp"

using Eigen::MatrixXd; using Eigen::VectorXd;
static const int NX=11, NZ=4;
static const double PI=3.14159265358979323846;
static double lim(double a){ while(a>PI)a-=2*PI; while(a<=-PI)a+=2*PI; return a; }

static VectorXd armor_xyz(const VectorXd&x,int id,int n){
  double ang=lim(x[6]+id*2*PI/n); bool L=(n==4)&&(id==1||id==3);
  double r=L? x[8]+x[9] : x[8];
  VectorXd o(3); o<<x[0]-r*cos(ang), x[2]-r*sin(ang), L? x[4]+x[10] : x[4]; return o;
}
static VectorXd ypd(const VectorXd&p){
  double x=p[0],y=p[1],z=p[2], d0=sqrt(x*x+y*y);
  VectorXd o(3); o<<atan2(y,x), atan2(z,d0), sqrt(d0*d0+z*z); return o;
}
static MatrixXd ypd_jac(const VectorXd&p){
  double x=p[0],y=p[1],z=p[2]; MatrixXd J(3,3);
  J(0,0)=-y/(x*x+y*y); J(0,1)=x/(x*x+y*y); J(0,2)=0;
  double q=pow(x*x+y*y,1.5)*(z*z/(x*x+y*y)+1);
  J(1,0)=-(x*z)/q; J(1,1)=-(y*z)/q; J(1,2)=1/((z*z/(x*x+y*y)+1)*sqrt(x*x+y*y));
  double d=sqrt(x*x+y*y+z*z); J(2,0)=x/d; J(2,1)=y/d; J(2,2)=z/d; return J;
}
static VectorXd h_of(const VectorXd&x,int id,int n){
  VectorXd a=armor_xyz(x,id,n), v=ypd(a); VectorXd z(4);
  z<<v[0],v[1],v[2],lim(x[6]+id*2*PI/n); return z;
}
static MatrixXd H_of(const VectorXd&x,int id,int n){
  double ang=lim(x[6]+id*2*PI/n); bool L=(n==4)&&(id==1||id==3);
  double r=L? x[8]+x[9] : x[8];
  MatrixXd A=MatrixXd::Zero(4,11);
  A(0,0)=1; A(0,6)=r*sin(ang);  A(0,8)=-cos(ang); A(0,9)=L?-cos(ang):0.0;
  A(1,2)=1; A(1,6)=-r*cos(ang); A(1,8)=-sin(ang); A(1,9)=L?-sin(ang):0.0;
  A(2,4)=1; A(2,10)=L?1.0:0.0; A(3,6)=1;
  MatrixXd J=ypd_jac(armor_xyz(x,id,n)); MatrixXd B=MatrixXd::Zero(4,4);
  B.block<3,3>(0,0)=J; B(3,3)=1; return B*A;
}
static VectorXd zsub(const VectorXd&a,const VectorXd&b){
  VectorXd c=a-b; c[0]=lim(c[0]); c[1]=lim(c[1]); c[3]=lim(c[3]); return c;
}
static MatrixXd Fm(double dt){ MatrixXd F=MatrixXd::Identity(11,11); F(0,1)=dt;F(2,3)=dt;F(4,5)=dt;F(6,7)=dt; return F; }
static MatrixXd Qm(double dt,double v1,double v2){
  double a=dt*dt*dt*dt/4,b=dt*dt*dt/2,c=dt*dt; MatrixXd Q=MatrixXd::Zero(11,11);
  int ix[4][2]={{0,1},{2,3},{4,5},{6,7}}; double vv[4]={v1,v1,v1,v2};
  for(int k=0;k<4;k++){int i=ix[k][0],j=ix[k][1];double v=vv[k];
    Q(i,i)=a*v;Q(i,j)=b*v;Q(j,i)=b*v;Q(j,j)=c*v;} return Q;
}
int main(){
  const double dt=1.0/60.0, v1=100.0, v2=75.0;
  const double r_yaw=4e-2,r_pitch=4e-2,r_base=1.5e-2, radius=0.2; const int n=4,id=0;
  VectorXd p0(11); p0<<1,64,1,64,1,64,0.4,400,1e-5,1,1;
  auto xadd=[](const VectorXd&a,const VectorXd&b){ VectorXd c=a+b; c[6]=lim(c[6]); return c; };

  std::mt19937 gen(7); std::normal_distribution<double> g(0,1);
  double maxdx=0, maxdP=0, sum_nis=0, sum_nis_old=0; int over=0, over_old=0, N=0;
  std::vector<int> runlen;            // 旧阈值下"Bad Converge"触发所需帧数

  MatrixXd F=Fm(dt), Q=Qm(dt,v1,v2);
  const int REP=120, STEPS=5200, BURN=200;
  for(int rep=0;rep<REP;rep++){
    VectorXd xt(11); xt<<5.0+g(gen)*0.5,0.8,1.5+g(gen)*0.5,-0.5,0.3,0.0,g(gen)*0.5,1.2,radius,0,0;
    VectorXd x0=xt; for(int i=0;i<11;i++) x0[i]+=g(gen)*sqrt(p0[i]);
    tools::ExtendedKalmanFilter ekn(x0,p0.asDiagonal(),xadd);        // 新（先验 NIS）
    tools_old::ExtendedKalmanFilter eko(x0,p0.asDiagonal(),xadd);    // 旧（后验 NIS）
    int fails=0; bool fired=false; int fired_at=-1;
    for(int k=0;k<STEPS;k++){
      double ax=g(gen)*sqrt(v1),ay=g(gen)*sqrt(v1),az=g(gen)*sqrt(v1),aa=g(gen)*sqrt(v2);
      xt[0]+=xt[1]*dt+0.5*dt*dt*ax; xt[1]+=dt*ax;
      xt[2]+=xt[3]*dt+0.5*dt*dt*ay; xt[3]+=dt*ay;
      xt[4]+=xt[5]*dt+0.5*dt*dt*az; xt[5]+=dt*az;
      xt[6]=lim(xt[6]+xt[7]*dt+0.5*dt*dt*aa); xt[7]+=dt*aa;

      VectorXd zt=h_of(xt,id,n);
      VectorXd ctr(3); ctr<<xt[0],xt[2],xt[4];
      double da=lim(xt[6]+id*2*PI/n - ypd(ctr)[0]);
      MatrixXd R=MatrixXd::Zero(4,4);
      R(0,0)=r_yaw; R(1,1)=r_pitch; R(2,2)=log(fabs(da)+1)+1; R(3,3)=log(zt[2]+1)/200+r_base;
      VectorXd z(4); for(int i=0;i<4;i++) z[i]=zt[i]+g(gen)*sqrt(R(i,i));

      auto h=[&](const VectorXd&x){ return h_of(x,id,n); };
      MatrixXd Hn=H_of(ekn.x,id,n), Ho=H_of(eko.x,id,n);
      ekn.predict(F,Q); eko.predict(F,Q);
      ekn.update(z,Hn,R,h,zsub); eko.update(z,Ho,R,h,zsub);

      // 逐帧比对：状态与协方差必须完全一致
      maxdx=std::max(maxdx,(ekn.x-eko.x).norm());
      maxdP=std::max(maxdP,(ekn.P-eko.P).norm());

      if(k>=BURN){ sum_nis+=ekn.data["nis"]; sum_nis_old+=eko.data["nis"];
        if(ekn.data["nis"]>9.488) over++; if(eko.data["nis"]>9.488) over_old++; N++; }
      // 旧阈值(0.711)下的 Bad Converge 触发时刻（窗口 10000、阈值 40%）
      if(eko.data["nis"]>0.711) fails++;
      if(!fired && fails>=0.4*10000){ fired=true; fired_at=k+1; }
    }
    if(fired_at>0) runlen.push_back(fired_at);
  }
  printf("逐帧比对 新旧 EKF： max|x_new-x_old| = %.3e , max|P_new-P_old| = %.3e\n", maxdx, maxdP);
  printf("新 NIS(先验): E=%.4f  P(>9.488)=%.2f%%   [理论: 4 与 5%%]\n", sum_nis/N, 100.0*over/N);
  printf("旧 NIS(后验): E=%.4f  P(>9.488)=%.2f%%\n", sum_nis_old/N, 100.0*over_old/N);
  if(!runlen.empty()){ sort(runlen.begin(),runlen.end());
    printf("旧阈值 0.711 触发 Bad-Converge 的帧数: 最小=%d 中位=%d 最大=%d (窗口10000/阈值40%%)\n",
      runlen.front(), runlen[runlen.size()/2], runlen.back()); }
  else printf("旧阈值 0.711 在 %d 帧内未触发 Bad-Converge\n", STEPS);
  return 0;
}
