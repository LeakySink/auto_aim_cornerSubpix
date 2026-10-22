// 验证：步兵(4装甲板,r=0.2,小陀螺)被误判为前哨站(3装甲板,r=0.2765)后，
// EKF 的一致性后果。两组对照：
//   A) 正确配置 armor_num=4, r0=0.2
//   B) 误判配置 armor_num=3, r0=0.2765   <- 现状
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using Eigen::MatrixXd; using Eigen::VectorXd;
static const double PI=3.14159265358979323846;
static double lim(double a){ while(a>PI)a-=2*PI; while(a<=-PI)a+=2*PI; return a; }

// 车体参数
static double R_TRUE=0.20, W_TRUE=2.0;   // 步兵真值：半径0.2m，小陀螺 2 rad/s
struct Cfg{ int n; double r; };

static VectorXd armor_xyz(const VectorXd&x,int id,int n,double r){
  double ang=lim(x[6]+id*2*PI/n);
  bool L=(n==4)&&(id==1||id==3);
  double rr=L? x[8]+x[9] : x[8];
  (void)r;
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
  VectorXd v=ypd(armor_xyz(x,id,n,0)); VectorXd z(4);
  z<<v[0],v[1],v[2],lim(x[6]+id*2*PI/n); return z;
}
static MatrixXd H_of(const VectorXd&x,int id,int n){
  double ang=lim(x[6]+id*2*PI/n); bool L=(n==4)&&(id==1||id==3);
  MatrixXd A=MatrixXd::Zero(4,11);
  A(0,0)=1; A(0,6)=x[8]*sin(ang);  A(0,8)=-cos(ang); A(0,9)=L?-cos(ang):0.0;
  A(1,2)=1; A(1,6)=-x[8]*cos(ang); A(1,8)=-sin(ang); A(1,9)=L?-sin(ang):0.0;
  A(2,4)=1; A(2,10)=L?1.0:0.0; A(3,6)=1;
  MatrixXd J=ypd_jac(armor_xyz(x,id,n,0)); MatrixXd B=MatrixXd::Zero(4,4);
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

// 一次完整运行，返回 NIS 统计
struct Res{ double mean; double p_over9488; double yaw_rms_deg; double r_end; };
static Res run(const Cfg&cfg,int vis,std::mt19937&gen){
  std::normal_distribution<double> g(0,1);
  const double dt=1.0/60.0, v1=100.0, v2=75.0;
  const double r_yaw=4e-2,r_pitch=4e-2,r_base=1.5e-2;
  // 真值状态：车体中心 + 半径 + 自转角速度
  double cx=5.0, cy=1.5, cz=0.3, yaw0=0.0, w=W_TRUE;
  // 滤波器状态（11维），按 cfg 初始化
  VectorXd x(11);
  x<<cx+R_TRUE*cos(yaw0),0,cy+R_TRUE*sin(yaw0),0,cz,0,yaw0,w,cfg.r,0,0;
  VectorXd p0d(11); p0d<<1,64,1,64,1,64,0.4,(cfg.n==3?100.0:400.0),1e-5,1,1;
  MatrixXd P=p0d.asDiagonal();
  MatrixXd F=Fm(dt), Q=Qm(dt,v1,v2);
  auto xadd=[](const VectorXd&a,const VectorXd&b){ VectorXd c=a+b; c[6]=lim(c[6]); return c; };
  int last_id=0;
  double sum_nis=0; int over=0, N=0, BURN=120;
  double sum_yaw2=0, cnt_yaw=0;
  for(int k=0;k<1500;k++){
    // 真值：车体原地小陀螺
    yaw0=lim(yaw0+w*dt);
    // 预测
    P=F*P*F.transpose()+Q; x=F*x; x[6]=lim(x[6]);
    // 观测：选朝向相机的 vis 块装甲板（车体yaw + id*90° 中最接近视线反向的）
    double los=atan2(cy,cx);
    int best=0; double bestd=1e9;
    for(int i=0;i<4;i++){ double d=std::abs(lim(lim(yaw0+i*PI/2)-los+PI)); if(d<bestd){bestd=d;best=i;} }
    std::vector<int> ids; ids.push_back(best);
    if(vis==2) ids.push_back((best+1)%4);
    for(int id_true: ids){
      VectorXd xt(11); xt<<cx,0,cy,0,cz,0,yaw0,w,R_TRUE,0,0;
      VectorXd zt=h_of(xt,id_true,4);   // 真值按 4 装甲板生成
      VectorXd ctr(3); ctr<<cx,cy,cz;
      double da=lim(lim(yaw0+id_true*PI/2)-ypd(ctr)[0]);
      MatrixXd R=MatrixXd::Zero(4,4);
      R(0,0)=r_yaw; R(1,1)=r_pitch; R(2,2)=log(std::abs(da)+1)+1; R(3,3)=log(zt[2]+1)/200+r_base;
      VectorXd z(4); for(int i=0;i<4;i++) z[i]=zt[i]+g(gen)*sqrt(R(i,i));
      // id 匹配（与 Target::update 同款 cost）
      double obs_yaw=z[3], obs_los=z[0], obs_dist=z[2];
      int id=-1; double minc=1e10; bool flip=false;
      for(int i=0;i<cfg.n;i++){
        VectorXd av=armor_xyz(x,i,cfg.n,0); VectorXd pv=ypd(av);
        double ang=lim(x[6]+i*2*PI/cfg.n);
        double ye=std::abs(lim(obs_yaw-ang)), yef=std::abs(lim(obs_yaw+PI-ang));
        double yc=std::min(ye,yef);
        double c=yc+0.5*std::abs(lim(obs_los-pv[0]))+0.2*std::abs(obs_dist-pv[2]);
        if(k>0 && i!=last_id && std::abs(x[7])<4.0) c+=0.35;
        if(c<minc){minc=c;id=i;flip=yef<ye;}
      }
      if(id<0) id=0;
      last_id=id;
      VectorXd zc=z; if(flip) zc[3]=lim(z[3]+PI);
      // EKF update
      auto h=[&](const VectorXd&xx){ return h_of(xx,id,cfg.n); };
      MatrixXd H=H_of(x,id,cfg.n);
      MatrixXd S=H*P*H.transpose()+R;
      VectorXd innov=zsub(zc,h(x));
      double nis=(innov.transpose()*S.ldlt().solve(innov)).value();
      MatrixXd K=S.ldlt().solve(H*P).transpose();
      x=xadd(x,K*innov); P=(MatrixXd::Identity(11,11)-K*H)*P*(MatrixXd::Identity(11,11)-K*H).transpose()+K*R*K.transpose();
      if(k>=BURN){ sum_nis+=nis; if(nis>9.488) over++; N++;
        sum_yaw2+=innov[3]*innov[3]; cnt_yaw++; }
    }
  }
  Res r; r.mean=sum_nis/N; r.p_over9488=100.0*over/N;
  r.yaw_rms_deg=sqrt(sum_yaw2/cnt_yaw)*180/PI; r.r_end=x[8];
  return r;
}
int main(){
  std::mt19937 gen(11);
  printf("场景：步兵真值 r=0.2m / 4块装甲板 / 小陀螺 w=2.0 rad/s，距离 5m，60fps\n");
  printf("观测按 4 装甲板真值生成；滤波器分别用【正确配置】与【误判为前哨站的配置】\n\n");
  printf("%-22s %10s %12s %14s %10s\n","滤波器配置","E[NIS]","P(>9.488)","yaw残差RMS(°)","收敛后r");
  struct{Cfg c;const char*nm;} cases[]={
    {{4,0.20},  "正确: n=4 r=0.20"},
    {{3,0.2765},"误判: n=3 r=0.2765"},
  };
  for(auto&cs: cases){
    int vis=1;
    Res r=run(cs.c,vis,gen);
    printf("%-22s %10.3f %11.2f%% %14.2f %10.4f\n",cs.nm,r.mean,r.p_over9488,r.yaw_rms_deg,r.r_end);
  }
  printf("\n(单帧仅 1 块装甲板可见，NIS 阈值 9.488 对应 5%% 名义误报率)\n");
  return 0;
}
