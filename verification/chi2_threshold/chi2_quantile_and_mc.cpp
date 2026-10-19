// ============================================================================
// 1) 精确计算 chi-square 分位数（正则化不完全 Gamma 函数）
// 2) 用 auto_aim 真实模型（11 维状态 / 4 维观测 / 真实 F,Q,H,R）跑蒙特卡洛，
//    检验"卡方阈值 0.711 -> 9.488"的必要性与正确性
// ============================================================================
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>
using namespace std;

// ---------- 小矩阵 ----------
struct M {
  int r, c; vector<double> a;
  M() : r(0), c(0) {}
  M(int r, int c) : r(r), c(c), a(r * c, 0.0) {}
  double & operator()(int i, int j) { return a[i * c + j]; }
  double operator()(int i, int j) const { return a[i * c + j]; }
};
static M mul(const M & A, const M & B) {
  M C(A.r, B.c);
  for (int i=0;i<A.r;i++) for (int k=0;k<A.c;k++) { double v=A(i,k); if(!v) continue;
    for (int j=0;j<B.c;j++) C(i,j) += v*B(k,j); }
  return C;
}
static M tr(const M & A) { M B(A.c,A.r); for(int i=0;i<A.r;i++) for(int j=0;j<A.c;j++) B(j,i)=A(i,j); return B; }
static M add(const M&A,const M&B){ M C(A.r,A.c); for(int i=0;i<(int)A.a.size();i++) C.a[i]=A.a[i]+B.a[i]; return C; }
static M sub(const M&A,const M&B){ M C(A.r,A.c); for(int i=0;i<(int)A.a.size();i++) C.a[i]=A.a[i]-B.a[i]; return C; }
static M eye(int n){ M I(n,n); for(int i=0;i<n;i++) I(i,i)=1; return I; }
static M solve(M A, M B) {   // A X = B
  int n=A.r;
  for (int k=0;k<n;k++){
    int p=k; for(int i=k+1;i<n;i++) if(fabs(A(i,k))>fabs(A(p,k))) p=i;
    if(p!=k){ for(int j=0;j<n;j++) swap(A(k,j),A(p,j)); for(int j=0;j<B.c;j++) swap(B(k,j),B(p,j)); }
    double d=A(k,k);
    for(int j=k;j<n;j++) A(k,j)/=d;
    for(int j=0;j<B.c;j++) B(k,j)/=d;
    for(int i=0;i<n;i++){ if(i==k) continue; double f=A(i,k); if(!f) continue;
      for(int j=k;j<n;j++) A(i,j)-=f*A(k,j);
      for(int j=0;j<B.c;j++) B(i,j)-=f*B(k,j); }
  }
  return B;
}
static double quad(const M & x, const M & S) { // x^T S^-1 x
  M y=solve(S,x); double s=0; for(int i=0;i<x.r;i++) s+=x(i,0)*y(i,0); return s;
}

// ---------- chi-square ----------
static double gammln(double x){
  static const double cof[6]={76.18009172947146,-86.50532032941677,24.01409824083091,
    -1.231739572450155,0.1208650973866179e-2,-0.5395239384953e-5};
  double y=x, tmp=x+5.5; tmp-=(x+0.5)*log(tmp); double ser=1.000000000190015;
  for(int j=0;j<6;j++) ser+=cof[j]/++y;
  return -tmp+log(2.5066282746310005*ser/x);
}
static double gser(double a,double x){
  const int ITMAX=2000; const double EPS=1e-14;
  double ap=a,sum=1.0/a,del=sum;
  for(int n=1;n<ITMAX;n++){ ap+=1; del*=x/ap; sum+=del; if(fabs(del)<fabs(sum)*EPS) break; }
  return sum*exp(-x+a*log(x)-gammln(a));
}
static double gcf(double a,double x){
  const int ITMAX=2000; const double EPS=1e-14, FPMIN=1e-300;
  double b=x+1-a, c=1/FPMIN, d=1/b, h=d;
  for(int i=1;i<ITMAX;i++){
    double an=-i*(i-a); b+=2; d=an*d+b; if(fabs(d)<FPMIN) d=FPMIN;
    c=b+an/c; if(fabs(c)<FPMIN) c=FPMIN; d=1/d; double del=d*c; h*=del;
    if(fabs(del-1.0)<EPS) break;
  }
  return exp(-x+a*log(x)-gammln(a))*h;
}
static double chi2_cdf(double x, int dof){         // P(X <= x)
  double a=0.5*dof, xx=0.5*x;
  return (xx < a+1.0) ? gser(a,xx) : 1.0-gcf(a,xx);
}
static double chi2_quantile(int dof, double p){    // 二分求分位数
  double lo=0, hi=1000;
  for(int i=0;i<200;i++){ double mid=0.5*(lo+hi); if(chi2_cdf(mid,dof)<p) lo=mid; else hi=mid; }
  return 0.5*(lo+hi);
}

// ---------- 真实模型 (复制自 target.cpp / math_tools.cpp) ----------
static const int NX=11, NZ=4;
static const double PI=3.14159265358979323846;
static double limit_rad(double a){ while(a>PI) a-=2*PI; while(a<=-PI) a+=2*PI; return a; }
static M F_mat(double dt){
  M F(NX,NX);
  for(int i=0;i<NX;i++) F(i,i)=1;
  F(0,1)=dt; F(2,3)=dt; F(4,5)=dt; F(6,7)=dt;
  return F;
}
static M Q_mat(double dt,double v1,double v2){
  double a=dt*dt*dt*dt/4, b=dt*dt*dt/2, c=dt*dt;
  M Q(NX,NX);
  Q(0,0)=a*v1; Q(0,1)=b*v1; Q(1,0)=b*v1; Q(1,1)=c*v1;
  Q(2,2)=a*v1; Q(2,3)=b*v1; Q(3,2)=b*v1; Q(3,3)=c*v1;
  Q(4,4)=a*v1; Q(4,5)=b*v1; Q(5,4)=b*v1; Q(5,5)=c*v1;
  Q(6,6)=a*v2; Q(6,7)=b*v2; Q(7,6)=b*v2; Q(7,7)=c*v2;
  return Q;
}
// h_armor_xyz
static void armor_xyz(const M&x,int id,int armor_num,M&out){
  double angle=limit_rad(x(6,0)+id*2*PI/armor_num);
  bool use_l_h=(armor_num==4)&&(id==1||id==3);
  double r=use_l_h? x(8,0)+x(9,0) : x(8,0);
  out=M(3,1);
  out(0,0)=x(0,0)-r*cos(angle);
  out(1,0)=x(2,0)-r*sin(angle);
  out(2,0)=use_l_h? x(4,0)+x(10,0) : x(4,0);
}
// xyz2ypd
static void xyz2ypd(const M&xyz,M&ypd){
  double x=xyz(0,0), y=xyz(1,0), z=xyz(2,0);
  double d0=sqrt(x*x+y*y);
  ypd=M(3,1);
  ypd(0,0)=atan2(y,x); ypd(1,0)=atan2(z,d0); ypd(2,0)=sqrt(d0*d0+z*z);
}
static void xyz2ypd_jac(const M&xyz,M&J){
  double x=xyz(0,0), y=xyz(1,0), z=xyz(2,0);
  J=M(3,3);
  J(0,0)=-y/(x*x+y*y); J(0,1)=x/(x*x+y*y);
  double p=pow(x*x+y*y,1.5)*(z*z/(x*x+y*y)+1);
  J(1,0)=-(x*z)/p; J(1,1)=-(y*z)/p; J(1,2)=1/((z*z/(x*x+y*y)+1)*sqrt(x*x+y*y));
  double d=sqrt(x*x+y*y+z*z);
  J(2,0)=x/d; J(2,1)=y/d; J(2,2)=z/d;
}
// h(x) -> z
static M h_of(const M&x,int id,int armor_num){
  M a; armor_xyz(x,id,armor_num,a);
  M ypd; xyz2ypd(a,ypd);
  M z(NZ,1);
  z(0,0)=ypd(0,0); z(1,0)=ypd(1,0); z(2,0)=ypd(2,0);
  z(3,0)=limit_rad(x(6,0)+id*2*PI/armor_num);
  return z;
}
static M h_jacobian(const M&x,int id,int armor_num){
  double angle=limit_rad(x(6,0)+id*2*PI/armor_num);
  bool use_l_h=(armor_num==4)&&(id==1||id==3);
  double r=use_l_h? x(8,0)+x(9,0) : x(8,0);
  double dx_da=r*sin(angle), dy_da=-r*cos(angle);
  double dx_dr=-cos(angle), dy_dr=-sin(angle);
  double dx_dl=use_l_h? -cos(angle):0.0, dy_dl=use_l_h? -sin(angle):0.0;
  double dz_dh=use_l_h?1.0:0.0;
  M A(NZ,NX);
  A(0,0)=1; A(0,6)=dx_da; A(0,8)=dx_dr; A(0,9)=dx_dl;
  A(1,2)=1; A(1,6)=dy_da; A(1,8)=dy_dr; A(1,9)=dy_dl;
  A(2,4)=1; A(2,10)=dz_dh;
  A(3,6)=1;
  M a; armor_xyz(x,id,armor_num,a);
  M J; xyz2ypd_jac(a,J);
  M B(NZ,NZ);
  B(0,0)=J(0,0); B(0,1)=J(0,1); B(0,2)=J(0,2);
  B(1,0)=J(1,0); B(1,1)=J(1,1); B(1,2)=J(1,2);
  B(2,0)=J(2,0); B(2,1)=J(2,1); B(2,2)=J(2,2);
  B(3,3)=1;
  return mul(B,A);
}
static M z_sub(const M&a,const M&b){
  M c=sub(a,b); c(0,0)=limit_rad(c(0,0)); c(1,0)=limit_rad(c(1,0)); c(3,0)=limit_rad(c(3,0)); return c;
}

static vector<double> g_nees, g_nis;
static long g_wrap=0, g_tail=0;
int main(){
  // ============ 第一部分：卡方分位数核验 ============
  printf("============ A. chi-square 分位数核验（自实现正则化不完全 Gamma） ============\n");
  printf("chi2(4):  CDF(0.711)  = %.6f   -> 0.711 是 %.1f%% 分位数\n", chi2_cdf(0.711,4), 100*chi2_cdf(0.711,4));
  printf("chi2(4):  5%%  分位数   = %.6f\n", chi2_quantile(4,0.05));
  printf("chi2(4):  50%% 分位数   = %.6f   (文档误称 0.711 是这个值)\n", chi2_quantile(4,0.50));
  printf("chi2(4):  95%% 分位数   = %.6f   (代码中 nis_threshold = 9.488)\n", chi2_quantile(4,0.95));
  printf("          1-CDF(9.488)= %.6f   (即 P(X>9.488) = 名义误报率)\n", 1-chi2_cdf(9.488,4));
  printf("chi2(11): 95%% 分位数   = %.6f   (代码中 nees_threshold = 19.675)\n", chi2_quantile(11,0.95));
  printf("          1-CDF(19.675)= %.6f\n", 1-chi2_cdf(19.675,11));
  printf("chi2(1):  50%% 分位数   = %.6f   (对照：0.711 也不是它)\n", chi2_quantile(1,0.50));
  printf("\n");

  // ============ 第二部分：真实滤波器蒙特卡洛 ============
  const double v1=100.0, v2=75.0;                 // q_xyz_acc_var / q_yaw_acc_var
  const double r_yaw=4.0e-2, r_pitch=4.0e-2, r_dist_base=1.5e-2;
  const double radius=0.2; const int armor_num=4; const int id=0;
  double p0dig[NX]={1,64,1,64,1,64,0.4,400,1.0e-5,1,1};

  const double dts[3]={1.0/30, 1.0/60, 1.0/120};
  const int    REP=300, STEPS=1200, BURN=200;

  printf("============ B. 真实 11 维 EKF 蒙特卡洛（%d 条轨迹 x %d 帧, 烧掉前 %d 帧） ============\n",
         REP, STEPS, BURN);
  printf("[dt]    E[NIS先验] P(>9.488)  E[NIS现行] P(>9.488) P(>0.711)  E[NEES现行] P(>19.675) E[NEES真值] P(>19.675)\n");

  for (int di=0; di<3; ++di) {
    double dt=dts[di];
    mt19937 gen(1234+di); normal_distribution<double> g(0.0,1.0);
    double s_prior=0, s_curr=0, n_curr=0, n_true=0;
    int c_prior=0, c_curr=0, c_curr_0711=0, c_nees=0, c_nees_true=0, N=0;

    for (int rep=0; rep<REP; ++rep) {
      // 真值初始化
      M xt(NX,1);
      xt(0,0)=5.0+g(gen)*0.5; xt(1,0)=0.8; xt(2,0)=1.5+g(gen)*0.5; xt(3,0)=-0.5;
      xt(4,0)=0.3; xt(5,0)=0.0; xt(6,0)=g(gen)*0.5; xt(7,0)=1.2;
      xt(8,0)=radius; xt(9,0)=0.0; xt(10,0)=0.0;
      // 滤波器初始化：真值 + P0 抽样偏差
      M x=xt; M P(NX,NX);
      for(int i=0;i<NX;i++){ double sd=sqrt(p0dig[i]); P(i,i)=p0dig[i]; x(i,0)+=g(gen)*sd; }
      M F=F_mat(dt), Q=Q_mat(dt,v1,v2);

      for (int k=0;k<STEPS;++k) {
        // --- 真值演化（与 Q 完全一致的分段白噪声模型）---
        {
          double ax=g(gen)*sqrt(v1), ay=g(gen)*sqrt(v1), az=g(gen)*sqrt(v1), aa=g(gen)*sqrt(v2);
          xt(0,0)+=xt(1,0)*dt+0.5*dt*dt*ax; xt(1,0)+=dt*ax;
          xt(2,0)+=xt(3,0)*dt+0.5*dt*dt*ay; xt(3,0)+=dt*ay;
          xt(4,0)+=xt(5,0)*dt+0.5*dt*dt*az; xt(5,0)+=dt*az;
          xt(6,0)=limit_rad(xt(6,0)+xt(7,0)*dt+0.5*dt*dt*aa); xt(7,0)+=dt*aa;
        }
        // --- 产生观测 ---
        M ztrue=h_of(xt,id,armor_num);
        // R（与 target.cpp 一致）
        M ctr(3,1); ctr(0,0)=xt(0,0); ctr(1,0)=xt(2,0); ctr(2,0)=xt(4,0);
        M ctr_ypd; xyz2ypd(ctr,ctr_ypd);
        double delta_angle=limit_rad(xt(6,0)+id*2*PI/armor_num - ctr_ypd(0,0));
        double R2=log(fabs(delta_angle)+1)+1;                 // 距离观测
        double R3=log(ztrue(2,0)+1)/200+r_dist_base;          // 角度观测
        M R(NZ,NZ); R(0,0)=r_yaw; R(1,1)=r_pitch; R(2,2)=R2; R(3,3)=R3;
        M z(NZ,1);
        for(int i=0;i<NZ;i++) z(i,0)=ztrue(i,0)+g(gen)*sqrt(R(i,i));

        // --- EKF predict ---
        P=add(mul(mul(F,P),tr(F)),Q);
        x=mul(F,x); x(6,0)=limit_rad(x(6,0));

        // --- EKF update ---
        M x_prior=x;
        M H=h_jacobian(x,id,armor_num);
        M S=add(mul(mul(H,P),tr(H)),R);
        M I4=eye(NZ), I11=eye(NX);
        M K=mul(mul(P,tr(H)),solve(S,I4));
        M innov=z_sub(z,h_of(x,id,armor_num));           // 先验新息
        double nis_prior=quad(innov,S);                  // 标准 NIS
        x=add(x,mul(K,innov)); x(6,0)=limit_rad(x(6,0));
        M IKH=sub(I11,mul(K,H));
        M Ppost=add(mul(mul(IKH,P),tr(IKH)),mul(mul(K,R),tr(K)));

        // 现行代码：后验残差 + 后验 S
        M rpost=z_sub(z,h_of(x,id,armor_num));
        M Spost=add(mul(mul(H,Ppost),tr(H)),R);
        double nis_curr=quad(rpost,Spost);
        // 现行 NEES
        M dx=sub(x,x_prior);
        double nees_curr=quad(dx,Ppost);
        // 真值 NEES（仅仿真可得，验证滤波器一致性）
        M dxt=sub(x,xt); dxt(6,0)=limit_rad(dxt(6,0));
        double nees_true=quad(dxt,Ppost);

        if (k>=BURN) {
          s_prior+=nis_prior; s_curr+=nis_curr; n_curr+=nees_curr; n_true+=nees_true;
          if(nis_prior>9.488) c_prior++;
          if(nis_curr>9.488) c_curr++;
          if(nis_curr>0.711) c_curr_0711++;
          if(nees_curr>19.675) c_nees++;
          if(di==1 && fabs(dx(6,0))>3.0) g_wrap++;
          if(di==1){ g_nees.push_back(nees_curr); g_nis.push_back(nis_curr); if(nees_curr>19.675) g_tail++; }
          if(nees_true>19.675) c_nees_true++;
          N++;
        }
        P=Ppost;
      }
    }
    printf("1/%-5d %9.3f %8.2f%%  %10.3f %8.2f%% %8.1f%%  %10.3f %8.2f%% %10.3f %8.2f%%\n",
      (int)(1/dt), s_prior/N, 100.0*c_prior/N, s_curr/N, 100.0*c_curr/N, 100.0*c_curr_0711/N,
      n_curr/N, 100.0*c_nees/N, n_true/N, 100.0*c_nees_true/N);
  }
  if(!g_nees.empty()){
    auto pc=[](vector<double>&v,double p){ sort(v.begin(),v.end()); return v[(int)(p*v.size())]; };
    printf("\n[dt=1/60] 现行 NEES 分位数: p50=%.3f p90=%.3f p99=%.3f max=%.1f\n", pc(g_nees,0.50), pc(g_nees,0.90), pc(g_nees,0.99), g_nees.back());
    printf("[dt=1/60] 现行 NIS  分位数: p50=%.3f p90=%.3f p95=%.3f p99=%.3f\n", pc(g_nis,0.50), pc(g_nis,0.90), pc(g_nis,0.95), pc(g_nis,0.99));
    printf("[dt=1/60] NEES 爆表帧 %ld 次, 其中 yaw 回绕帧(|dx[6]|>3) %ld 次, 占比 %.1f%%\n", g_tail, g_wrap, 100.0*g_wrap/g_tail);
    printf("[dt=1/60] 旧阈值 0.711 落在 NIS 的第 %.2f%% 分位 -> 单帧误报率 %.1f%%\n", 100.0*(lower_bound(g_nis.begin(),g_nis.end(),0.711)-g_nis.begin())/g_nis.size(), 100.0-100.0*(lower_bound(g_nis.begin(),g_nis.end(),0.711)-g_nis.begin())/g_nis.size());
  }
  printf("\n列含义：NIS先验=标准卡方统计量(chi2(4)); NIS现行=代码当前用后验量算出的值\n");
  return 0;
}
