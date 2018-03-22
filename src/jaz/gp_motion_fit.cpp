#include <src/jaz/gp_motion_fit.h>
#include <src/jaz/svd_helper.h>
#include <src/jaz/interpolation.h>

using namespace gravis;

GpMotionFit::GpMotionFit(
    const std::vector<std::vector<Image<RFLOAT>>>& correlation,
    double sig_vel_px, double sig_div_px, double sig_acc_px,
    int maxDims,
    const std::vector<d2Vector>& positions,
    const std::vector<d2Vector>& perFrameOffsets,
    int threads, bool expKer)
:
    expKer(expKer),
    pc(correlation.size()),
    fc(correlation[0].size()),
    threads(threads),
    sig_vel_px(sig_vel_px),
    sig_div_px(sig_div_px),
    sig_acc_px(sig_acc_px),
    correlation(correlation),
    positions(positions),
    perFrameOffsets(perFrameOffsets)
{
    Matrix2D<RFLOAT> A(pc,pc);

    const double sv2 = sig_vel_px * sig_vel_px;
    const double sd2 = sig_div_px * sig_div_px;

    for (int i = 0; i < pc; i++)
    for (int j = i; j < pc; j++)
    {
        const double dd = (positions[i] - positions[j]).norm2();
        const double k = sv2 * (expKer? exp(-sqrt(dd/sd2)) : exp(-0.5*dd/sd2));
        A(i,j) = k;
        A(j,i) = k;
    }

    Matrix2D<RFLOAT> U, Vt;
    Matrix1D<RFLOAT> S;

    SvdHelper::decompose(A, U, S, Vt);

    dc = (maxDims > pc)? pc : maxDims;

    basis = Matrix2D<RFLOAT>(pc,dc);

    for (int d = 0; d < dc; d++)
    {
        const double l = sqrt(S(d));

        for (int p = 0; p < pc; p++)
        {
            basis(p,d) = l * Vt(p,d);
        }
    }

    eigenVals = std::vector<double>(dc);

    for (int d = 0; d < dc; d++)
    {
        eigenVals[d] = S(d);
    }
}

double GpMotionFit::f(const std::vector<double> &x) const
{
    std::vector<std::vector<d2Vector>> pos(pc, std::vector<d2Vector>(fc));
    paramsToPos(x, pos);

    double e_tot = 0.0;

    #pragma omp parallel for num_threads(threads)
    for (int p = 0; p < pc; p++)
    {
        double e = 0.0;

        for (int f = 0; f < fc; f++)
        {
            e -= Interpolation::cubicXY(correlation[p][f],
                    pos[p][f].x + perFrameOffsets[f].x,
                    pos[p][f].y + perFrameOffsets[f].y, 0, 0, true);
        }

        #pragma omp atomic
            e_tot += e;
    }

    for (int f = 0; f < fc-1; f++)
    for (int d = 0; d < dc; d++)
    {
        const double cx = x[2*(pc + dc*f + d)    ];
        const double cy = x[2*(pc + dc*f + d) + 1];

        e_tot += cx*cx + cy*cy;
    }

    if (sig_acc_px > 0.0)
    {
        for (int f = 0; f < fc-2; f++)
        for (int d = 0; d < dc; d++)
        {
            const double cx0 = x[2*(pc + dc*f + d)    ];
            const double cy0 = x[2*(pc + dc*f + d) + 1];
            const double cx1 = x[2*(pc + dc*(f+1) + d)    ];
            const double cy1 = x[2*(pc + dc*(f+1) + d) + 1];

            const double dcx = cx1 - cx0;
            const double dcy = cy1 - cy0;

            e_tot += eigenVals[d]*(dcx*dcx + dcy*dcy) / (sig_acc_px*sig_acc_px);
        }
    }

    return e_tot;
}

void GpMotionFit::grad(const std::vector<double> &x,
                       std::vector<double> &gradDest) const
{
    std::vector<std::vector<d2Vector>> pos(pc, std::vector<d2Vector>(fc));
    paramsToPos(x, pos);

    std::vector<std::vector<d2Vector>> ccg_pf(pc, std::vector<d2Vector>(fc));

    for (int p = 0; p < pc; p++)
    {
        for (int f = 0; f < fc; f++)
        {
            ccg_pf[p][f] = Interpolation::cubicXYgrad(
                correlation[p][f],
                pos[p][f].x + perFrameOffsets[f].x,
                pos[p][f].y + perFrameOffsets[f].y,
                0, 0, true);
        }
    }

    for (int i = 0; i < gradDest.size(); i++)
    {
        gradDest[i] = 0.0;
    }

    for (int p = 0; p < pc; p++)
    for (int f = 0; f < fc; f++)
    {
        gradDest[2*p  ] += ccg_pf[p][f].x;
        gradDest[2*p+1] += ccg_pf[p][f].y;
    }

    for (int d = 0; d < dc; d++)
    for (int p = 0; p < pc; p++)
    {
        d2Vector g(0.0, 0.0);

        for (int f = fc-2; f >= 0; f--)
        {
            g.x += basis(p,d) * ccg_pf[p][f].x;
            g.y += basis(p,d) * ccg_pf[p][f].y;

            gradDest[2*(pc + dc*f + d)  ] -= g.x;
            gradDest[2*(pc + dc*f + d)+1] -= g.y;
        }
    }

    for (int f = 0; f < fc-1; f++)
    for (int d = 0; d < pc; d++)
    {
        gradDest[2*(pc + dc*f + d)  ] += 2.0 * x[2*(pc + dc*f + d)  ];
        gradDest[2*(pc + dc*f + d)+1] += 2.0 * x[2*(pc + dc*f + d)+1];
    }

    if (sig_acc_px > 0.0)
    {
        const double sa2 = sig_acc_px*sig_acc_px;

        for (int f = 0; f < fc-2; f++)
        for (int d = 0; d < dc; d++)
        {
            const double cx0 = x[2*(pc + dc*f + d)    ];
            const double cy0 = x[2*(pc + dc*f + d) + 1];
            const double cx1 = x[2*(pc + dc*(f+1) + d)    ];
            const double cy1 = x[2*(pc + dc*(f+1) + d) + 1];

            const double dcx = cx1 - cx0;
            const double dcy = cy1 - cy0;

            //e_tot += eigenVals[d]*(dcx*dcx + dcy*dcy) / (sig_acc_px*sig_acc_px);

            gradDest[2*(pc + dc*f + d)  ] -= 2.0 * eigenVals[d] * dcx / sa2;
            gradDest[2*(pc + dc*f + d)+1] -= 2.0 * eigenVals[d] * dcy / sa2;
            gradDest[2*(pc + dc*(f+1) + d)  ] += 2.0 * eigenVals[d] * dcx / sa2;
            gradDest[2*(pc + dc*(f+1) + d)+1] += 2.0 * eigenVals[d] * dcy / sa2;
        }
    }
}

void GpMotionFit::paramsToPos(
    const std::vector<double>& x,
    std::vector<std::vector<d2Vector>>& pos) const
{
    for (int p = 0; p < pc; p++)
    {
        d2Vector pp(x[2*p], x[2*p+1]);

        for (int f = 0; f < fc; f++)
        {
            pos[p][f] = pp;

            if (f < fc-1)
            {
                d2Vector vel(0.0, 0.0);

                for (int d = 0; d < dc; d++)
                {
                    const double cx = x[2*(pc + dc*f + d)    ];
                    const double cy = x[2*(pc + dc*f + d) + 1];

                    vel.x += cx * basis(p,d);
                    vel.y += cy * basis(p,d);
                }

                pp += vel;
            }
        }
    }
}

void GpMotionFit::posToParams(
    const std::vector<std::vector<d2Vector>>& pos,
    std::vector<double>& x) const
{
    for (int p = 0; p < pc; p++)
    {
        x[2*p]   = pos[p][0].x;
        x[2*p+1] = pos[p][0].y;
    }

    for (int f = 0; f < fc-1; f++)
    for (int d = 0; d < dc; d++)
    {
        d2Vector c(0.0, 0.0);

        for (int p = 0; p < pc; p++)
        {
            d2Vector v = pos[p][f+1] - pos[p][f];

            c.x += v.x * basis(p,d);
            c.y += v.y * basis(p,d);
        }

        x[2*(pc + dc*f + d)  ] = c.x/eigenVals[d];
        x[2*(pc + dc*f + d)+1] = c.y/eigenVals[d];
    }
}
