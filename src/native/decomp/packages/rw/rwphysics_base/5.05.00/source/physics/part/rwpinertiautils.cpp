// rw::physics inertia / eigen helpers (rwphysics_base rwppart unity build).
#include <rw/math/vpu.h>
using namespace rw::math::vpu;

namespace rw { namespace physics {

// MATCH sk83_na_m 0x82ce1968
void sym_schur2(Matrix33 &a, unsigned int p, unsigned int q, float &s, float &c)
{
    c = 1.0f;
    s = 0.0f;
    if (a(p, q) != VecFloat(0.0f))
    {
        float tau = (a(q, q) - a(p, p)) * VecFloat(0.5f) / a(p, q);
        float t;
        if (tau > 0.0f)
            t = 1.0f / (tau + sqrtf(tau * tau + 1.0f));
        else
            t = -1.0f / (sqrtf(tau * tau + 1.0f) - tau);
        c = 1.0f / sqrtf(t * t + 1.0f);
        s = t * c;
    }
}

// MATCH sk83_na_m 0x82ce1ac0
void jacobi_update_cols(Matrix33 &a, unsigned int p, unsigned int q, float c, float s)
{
    float t0p = a(0, p) * c - a(0, q) * s;
    float t1p = a(1, p) * c - a(1, q) * s;
    float t2p = a(2, p) * c - a(2, q) * s;
    float t0q = a(0, p) * s + a(0, q) * c;
    float t1q = a(1, p) * s + a(1, q) * c;
    float t2q = a(2, p) * s + a(2, q) * c;
    a(0, p) = t0p;
    a(1, p) = t1p;
    a(2, p) = t2p;
    a(0, q) = t0q;
    a(1, q) = t1q;
    a(2, q) = t2q;
}

// MATCH sk83_na_m 0x82ce1e60
void jacobi_update_rows(Matrix33 &a, unsigned int p, unsigned int q, float c, float s)
{
    float tp0 = a(p, 0) * c - a(q, 0) * s;
    float tp1 = a(p, 1) * c - a(q, 1) * s;
    float tp2 = a(p, 2) * c - a(q, 2) * s;
    float tq0 = a(p, 0) * s + a(q, 0) * c;
    float tq1 = a(p, 1) * s + a(q, 1) * c;
    float tq2 = a(p, 2) * s + a(q, 2) * c;
    a(p, 0) = tp0;
    a(p, 1) = tp1;
    a(p, 2) = tp2;
    a(q, 0) = tq0;
    a(q, 1) = tq1;
    a(q, 2) = tq2;
}

// PARTIAL 188/222 sk83_na_m 0x82ce2218 (identity-init prologue: register allocation only)
unsigned int JacobiRowCyclic(Matrix33 &a, Matrix33 &v, unsigned int maxIterations, float tolerance)
{
    v(0, 0) = 1.0f;
    v(1, 1) = 1.0f;
    v(2, 2) = 1.0f;
    v(1, 0) = 0.0f;
    v(2, 0) = 0.0f;
    v(2, 1) = 0.0f;
    v(0, 1) = 0.0f;
    v(0, 2) = 0.0f;
    v(1, 2) = 0.0f;

    while (a(1, 0) * a(1, 0) + a(2, 0) * a(2, 0) + a(2, 1) * a(2, 1) >
               ((a(0, 0) * a(0, 0) < a(1, 1) * a(1, 1))
                    ? ((a(0, 0) * a(0, 0) < a(2, 2) * a(2, 2)) ? a(0, 0) * a(0, 0) : a(2, 2) * a(2, 2))
                    : ((a(1, 1) * a(1, 1) < a(2, 2) * a(2, 2)) ? a(1, 1) * a(1, 1) : a(2, 2) * a(2, 2)))
                   * VecFloat(tolerance * tolerance)
           && maxIterations != 0)
    {
        for (int p = 1; p < 3; p++)
        {
            for (int q = 0; q < p; q++)
            {
                float s, c;
                sym_schur2(a, p, q, s, c);
                jacobi_update_cols(v, p, q, c, s);
                jacobi_update_cols(a, p, q, c, s);
                jacobi_update_rows(a, p, q, c, s);
            }
        }
        maxIterations--;
    }
    return maxIterations;
}

} }
