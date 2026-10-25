// 场景：步兵真值(4块装甲板/r=0.2/小陀螺)，被误判为前哨站(3块/r=0.2765+转速钳位)
// 评估三项：NIS 一致性、预测 yaw 滞后、瞄准点误差（弹丸飞行时间后的装甲板位置偏差）
//
// 读表注意：瞄准点误差列受状态估计噪声主导，不是"误判"的净效应。
// 生产 target.cpp 的 R(2,2)=log(|Δangle|+1)+1 对应测距 σ≈1 m，蒙特卡洛平均后
// 剩余误差仍在 0.5 m 量级（装甲板半宽只有 6.75 cm），所以正确的 n=4 配置这列
// 也不为 0，两栏之间看不出差别。要比较配置只能看 NIS / 滞后角两列，
// 或把 R 缩到实测噪声后再跑。DIAGNOSIS.md 的结论只用了前两列。
#include <Eigen/Dense>
#include <cmath>
#include <cstdio>
#include <random>
using Eigen::MatrixXd; using Eigen::VectorXd;
static const double PI=3.14159265358979323846;
static double lim(double a){ while(a>PI)a-=2*PI; while(a<=-PI)a+=2*PI; return a; }
struct Cfg{ int n; double r; bool clamp; const char* nm; };

static VectorXd armor_xyz(const VectorXd&x,int id,int n){
  double ang=lim(x[6]+id*2*PI/n); bool L=(n==4)&&(id==1||id==3);
  double rr=L? x[8]+x[9] : x[8];
  VectorXd o(3); o<<x[0]-rr*cos(ang), x[2]-rr*sin(ang), L? x[4]+x[10] : x[4]; return o;
}
static VectorXd ypd(const VectorXd&p){
  double x=p[0],y=p[1],z=p[2],d0=sqrt(x*x+y*y);
  VectorXd o(3); o<<atan2(y,x),atan2(z,d0),sqrt(d0*d0+z*z); return o;
}
static MatrixXd ypd_jac(const VectorXd&p){
  double x=p[0],y=p[1],z=p[2]; MatrixXd J(3,3);
  J(0,0)=-y/(x*x+y*y); J(0,1)=x/(x*x+y*y); J(0,2)=0;
  double q=pow(x*x+y*y,1.5)*(z*z/(x*x+y*y)+1);
  J(1,0)=-(x*z)/q; J(1,1)=-(y*z)/q; J(1,2)=1/((z*z/(x*x+y*y)+1)*sqrt(x*x+y*y));
  double d=sqrt(x*x+y*y+z*z); J(2,0)=x/d; J(2,1)=y/d; J(2,2)=z/d; return J;
}
static VectorXd h_of(const VectorXd&x,int id,int n){
  VectorXd v=ypd(armor_xyz(x,id,n)); VectorXd z(4);
  z<<v[0],v[1],v[2],lim(x[6]+id*2*PI/n); return z;
}
static MatrixXd H_of(const VectorXd&x,int id,int n){
  double ang=lim(x[6]+id*2*PI/n); bool L=(n==4)&&(id==1||id==3);
  MatrixXd A=MatrixXd::Zero(4,11);
  A(0,0)=1; A(0,6)=x[8]*sin(ang);  A(0,8)=-cos(ang); A(0,9)=L?-cos(ang):0.0;
  A(1,2)=1; A(1,6)=-x[8]*cos(ang); A(1,8)=-sin(ang); A(1,9)=L?-sin(ang):0.0;
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
struct Res{ double nis, over, yaw_lag_deg, aim_err_m; };
static Res run(const Cfg&cfg,double w_true,std::mt19937&gen){
  std::normal_distribution<double> g(0,1);
  const double dt=1.0/60.0, v1=100.0, v2=75.0;
  const double r_yaw=4e-2,r_pitch=4e-2,r_base=1.5e-2;
  const double R_TRUE=0.20, cx=5.0, cy=1.5, cz=0.3;
  const double TOF=0.20;                       // 弹丸飞行时间 ~5m/25m/s
  double yaw_true=0.0;
  VectorXd x(11); x<<cx+R_TRUE,0,cy,0,cz,0,0,w_true,cfg.r,0,0;
  VectorXd p0d(11); p0d<<1,64,1,64,1,64,0.4,(cfg.n==3?100.0:400.0),1e-5,1,1;
  MatrixXd P=p0d.asDiagonal();
  MatrixXd F=Fm(dt), Q=Qm(dt,v1,v2);
  auto xadd=[](const VectorXd&a,const VectorXd&b){ VectorXd c=a+b; c[6]=lim(c[6]); return c; };
  int last_id=0; double s_nis=0; int over=0,N=0,BURN=200;
  double s_lag=0, s_aim=0, cnt=0;
  for(int k=0;k<2000;k++){
    yaw_true=lim(yaw_true+w_true*dt);
    P=F*P*F.transpose()+Q; x=F*x; x[6]=lim(x[6]);
    bool conv = (k> (cfg.n==3?10:3));           // convergened(): outpost>10, 其它>3
    if(cfg.clamp && conv && std::abs(x[7])>2) x[7]= x[7]>0?2.51:-2.51;
    // 观测：取最朝向相机的那块（真值 4 块）
    double los=atan2(cy,cx); int best=0; double bd=1e9;
    for(int i=0;i<4;i++){ double d=std::abs(lim(lim(yaw_true+i*PI/2)-los+PI)); if(d<bd){bd=d;best=i;} }
    VectorXd xt(11); xt<<cx,0,cy,0,cz,0,yaw_true,w_true,R_TRUE,0,0;
    VectorXd zt=h_of(xt,best,4);
    VectorXd ctr(3); ctr<<cx,cy,cz;
    double da=lim(lim(yaw_true+best*PI/2)-ypd(ctr)[0]);
    MatrixXd R=MatrixXd::Zero(4,4);
    R(0,0)=r_yaw;R(1,1)=r_pitch;R(2,2)=log(std::abs(da)+1)+1;R(3,3)=log(zt[2]+1)/200+r_base;
    VectorXd z(4); for(int i=0;i<4;i++) z[i]=zt[i]+g(gen)*sqrt(R(i,i));
    // id 匹配
    int id=-1; double mc=1e10; bool flip=false;
    for(int i=0;i<cfg.n;i++){
      VectorXd pv=ypd(armor_xyz(x,i,cfg.n)); double ang=lim(x[6]+i*2*PI/cfg.n);
      double ye=std::abs(lim(z[3]-ang)), yef=std::abs(lim(z[3]+PI-ang)), yc=std::min(ye,yef);
      double c=yc+0.5*std::abs(lim(z[0]-pv[0]))+0.2*std::abs(z[2]-pv[2]);
      if(k>0&&i!=last_id&&std::abs(x[7])<4.0) c+=0.35;
      if(c<mc){mc=c;id=i;flip=yef<ye;}
    }
    if(id<0) id=0;
      last_id=id;
    VectorXd zc=z; if(flip) zc[3]=lim(z[3]+PI);
    auto h=[&](const VectorXd&xx){ return h_of(xx,id,cfg.n); };
    MatrixXd H=H_of(x,id,cfg.n); MatrixXd S=H*P*H.transpose()+R;
    VectorXd innov=zsub(zc,h(x));
    double nis=(innov.transpose()*S.ldlt().solve(innov)).value();
    MatrixXd K=S.ldlt().solve(H*P).transpose();
    x=xadd(x,K*innov);
    P=(MatrixXd::Identity(11,11)-K*H)*P*(MatrixXd::Identity(11,11)-K*H).transpose()+K*R*K.transpose();
    if(k>=BURN){
      s_nis+=nis; if(nis>9.488) over++; N++;
      // 预测滞后：滤波器 x[7] 与真值 w_true 之差，乘飞行时间
      s_lag += std::abs(lim((w_true-x[7])*TOF))*180/PI;
      // 瞄准点误差：TOF 秒后真值装甲板位置 vs 滤波器预测的装甲板位置
      double yt = lim(yaw_true + w_true*TOF);
      double yf = lim(x[6] + x[7]*TOF);
      // 真值：取 TOF 后最朝向相机的装甲板
      int bt=0; double bdt=1e9;
      for(int i=0;i<4;i++){ double d=std::abs(lim(lim(yt+i*PI/2)-los+PI)); if(d<bdt){bdt=d;bt=i;} }
      Eigen::Vector3d pt{cx-R_TRUE*cos(lim(yt+bt*PI/2)), cy-R_TRUE*sin(lim(yt+bt*PI/2)), cz};
      // 滤波器：取其选中的装甲板预测位置
      int bf=0; double bdf=1e9;
      for(int i=0;i<cfg.n;i++){ double d=std::abs(lim(lim(yf+i*2*PI/cfg.n)-los+PI)); if(d<bdf){bdf=d;bf=i;} }
      // 位置必须与选板用同一个"TOF 后的 yaw"。原写法 armor_xyz(x,...) 用的是当前
      // x[6]，等于"用当前姿态预测未来位置"，即使滤波器无噪声也会残留
      // 2r·sin(w·TOF/2) 的固定偏差（w=5 时 0.19m），并且 w 越大偏差越大，
      // 会让"误判配置"反而显得更准。
      VectorXd xf=x; xf[6]=yf;
      Eigen::Vector3d pf=armor_xyz(xf,bf,cfg.n);
      s_aim += (pt-pf).norm(); cnt++;
    }
  }
  Res r; r.nis=s_nis/N; r.over=100.0*over/N; r.yaw_lag_deg=s_lag/cnt; r.aim_err_m=s_aim/cnt;
  return r;
}
int main(){
  std::mt19937 gen(2024);
  printf("真值：步兵 4 块装甲板 / r=0.20m / 距离 5m / 60fps / 弹丸飞行时间 0.20s\n");
  printf("装甲板宽 0.135m —— 瞄准误差超过约 0.07m 即大概率脱靶\n\n");
  printf("%-10s | %-26s | %8s %10s %12s %12s\n","真值转速","滤波器配置","E[NIS]","P(>9.488)","预测滞后(°)","瞄准误差(m)");
  printf("-----------|----------------------------|----------|------------|------------|------------\n");
  double ws[3]={2.0,5.0,10.0};
  Cfg correct{4,0.20,false,"正确: n=4 r=0.20"};
  Cfg wrong  {3,0.2765,true ,"误判: n=3 r=0.2765+钳位"};
  for(double w:ws){
    Res a=run(correct,w,gen);
    printf("%-10s | %-26s | %8.2f %9.2f%% %11.2f %11.4f\n",
      (std::to_string(w)+" rad/s").c_str(),correct.nm,a.nis,a.over,a.yaw_lag_deg,a.aim_err_m);
    Res b=run(wrong,w,gen);
    printf("%-10s | %-26s | %8.2f %9.2f%% %11.2f %11.4f\n",
      "",wrong.nm,b.nis,b.over,b.yaw_lag_deg,b.aim_err_m);
    printf("-----------|----------------------------|----------|------------|------------|------------\n");
  }
  return 0;
}
