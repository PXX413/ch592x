/**************************************************************************
 * ui_anim.c —— 轻量动画引擎实现
 *
 * 全部定点运算，无浮点、无动态分配。
 * 进度统一用 Q8：0..256 表示 0%..100%。
 **************************************************************************/

#include "ui_anim.h"
#include "tft_gfx.h"        /* Anim_FadeRect 需要 TFT_FillRect */

/*==========================================================================
 * 内部小工具
 *========================================================================*/

/* 定点乘法：a * b / 256，四舍五入。
 * a 最大 240*256，b 最大 256，乘积 15.7M —— 用 int32 安全。 */
static int16_t mul256(int16_t a, int16_t b)
{
    int32_t r = (int32_t)a * (int32_t)b;
    if (r >= 0) { r += 128; }
    else        { r -= 128; }
    return (int16_t)(r / 256);
}

/* 限幅 */
static int16_t clamp16(int32_t v, int16_t lo, int16_t hi)
{
    if (v < (int32_t)lo) { return lo; }
    if (v > (int32_t)hi) { return hi; }
    return (int16_t)v;
}

/*==========================================================================
 * 1. 缓动函数
 *
 * 输入 t ∈ [0,256]，输出 ∈ [0,256]（BACK/BOUNCE 会短暂越界，属预期）
 *========================================================================*/
int16_t Anim_Ease(ease_t e, int16_t t)
{
    int32_t x;

    if (t <= 0)   { return 0; }
    if (t >= 256) { return 256; }

    switch (e)
    {
        case EASE_LINEAR:
            return t;

        case EASE_IN:
            /* t^2 / 256 */
            return (int16_t)(((int32_t)t * t) / 256);

        case EASE_OUT:
            /* 1-(1-t)^2 —— 快出慢停，最自然 */
            x = 256 - t;
            return (int16_t)(256 - ((x * x) / 256));

        case EASE_INOUT:
            /* 前半段加速、后半段减速：4t^3 形式的分段近似 */
            if (t < 128)
            {
                return (int16_t)((2 * (int32_t)t * t) / 256);
            }
            else
            {
                x = 256 - t;
                return (int16_t)(256 - (2 * (x * x) / 256));
            }

        case EASE_OUT_BACK:
        {
            /* 回弹：先冲过头再收回来。
             * 标准 back-out: 1 + c3*(t-1)^3 + c1*(t-1)^2, c1=1.70158
             * 这里用 1/256 精度近似 c1=1.5625 (400/256) */
            int32_t u = t - 256;                 /* u ∈ [-256,0] */
            int32_t u2 = (u * u) / 256;          /* u^2/256 */
            int32_t u3 = (u2 * u) / 256;         /* u^3/256^2 */
            /* 256 + 1.5625*u3 + 1.5625*u2  -> 用 400/256 缩放 */
            int32_t r = 256 + (400 * (u3 + u2)) / 256;
            return (int16_t)r;
        }

        case EASE_OUT_BOUNCE:
        {
            /* 分段抛物线弹跳（经典 bounce-out 近似） */
            int32_t ti = t;
            if (ti < 128)
            {
                /* 前半段：正常减速 */
                int32_t u = 128 - ti;
                return (int16_t)(128 - (u * u) / 128);
            }
            else if (ti < 224)
            {
                /* 第一次弹起 */
                int32_t u = ti - 128;
                /* 峰高 1/2，宽度 96 */
                int32_t h = 64 - ((u - 48) * (u - 48) * 64) / (48 * 48);
                if (h < 0) { h = 0; }
                return (int16_t)(192 + h);
            }
            else
            {
                /* 第二次小弹起 */
                int32_t u = ti - 224;
                int32_t h = 32 - ((u - 16) * (u - 16) * 32) / (16 * 16);
                if (h < 0) { h = 0; }
                return (int16_t)(224 + h);
            }
        }

        default:
            return t;
    }
}

/*==========================================================================
 * 2. 补间
 *========================================================================*/
void Anim_Start(anim_t *a, int16_t from, int16_t to, uint16_t dur_ms, ease_t e)
{
    if (a == 0) { return; }

    a->from   = from;
    a->to     = to;
    a->cur    = from;
    a->dur_ms = dur_ms;
    a->el_ms  = 0u;
    a->ease   = (uint8_t)e;

    if ((dur_ms == 0u) || (from == to))
    {
        /* 无需动画，直接到位 */
        a->cur    = to;
        a->active = 0u;
    }
    else
    {
        a->active = 1u;
    }
}

void Anim_To(anim_t *a, int16_t to, uint16_t dur_ms, ease_t e)
{
    if (a == 0) { return; }
    Anim_Start(a, a->cur, to, dur_ms, e);
}

bool Anim_Step(anim_t *a, uint16_t dt_ms)
{
    int16_t t;
    int16_t p;

    if ((a == 0) || (a->active == 0u)) { return false; }

    /* 累加时间（防溢出：dur_ms 是 uint16，el 也限幅到 dur） */
    if ((uint32_t)a->el_ms + dt_ms >= (uint32_t)a->dur_ms)
    {
        a->el_ms  = a->dur_ms;
        a->cur    = a->to;
        a->active = 0u;
        return false;               /* 本帧已到位，无需再重绘 */
    }

    a->el_ms = (uint16_t)((uint32_t)a->el_ms + dt_ms);

    /* 线性进度 -> 缓动进度 */
    t = (int16_t)(((int32_t)a->el_ms * 256) / (int32_t)a->dur_ms);
    p = Anim_Ease((ease_t)a->ease, t);

    a->cur = (int16_t)(a->from + mul256((int16_t)(a->to - a->from), p));
    return true;
}

void Anim_Jump(anim_t *a)
{
    if (a == 0) { return; }
    a->cur    = a->to;
    a->el_ms  = a->dur_ms;
    a->active = 0u;
}

void Anim_Reset(anim_t *a)
{
    if (a == 0) { return; }
    a->cur    = a->from;
    a->el_ms  = 0u;
    a->active = 1u;
}

int16_t Anim_Value(const anim_t *a)
{
    return (a != 0) ? a->cur : 0;
}

bool Anim_Active(const anim_t *a)
{
    return (a != 0) && (a->active != 0u);
}

int16_t Anim_Lerp(int16_t from, int16_t to, int16_t prog256)
{
    int16_t p = clamp16(prog256, 0, 256);
    return (int16_t)(from + mul256((int16_t)(to - from), p));
}

/*==========================================================================
 * 3. 页面转场
 *
 * prog 用 0..240（屏宽）当进度量，正好等于"新页已进入的像素数"。
 *========================================================================*/
void Anim_TransStart(ui_trans_t *t, trans_dir_t dir, uint16_t dur_ms)
{
    if (t == 0) { return; }

    t->dir    = (uint8_t)dir;
    t->phase  = 0u;
    Anim_Start(&t->prog, 0, 240, dur_ms, EASE_OUT);
    t->active = 1u;
}

bool Anim_TransStep(ui_trans_t *t, uint16_t dt_ms)
{
    if ((t == 0) || (t->active == 0u)) { return false; }

    if (!Anim_Step(&t->prog, dt_ms))
    {
        t->active = 0u;
        t->phase  = 1u;             /* 新页完全接管 */
        return false;
    }
    return true;
}

bool Anim_TransActive(const ui_trans_t *t)
{
    return (t != 0) && (t->active != 0u);
}

int16_t Anim_TransOldX(const ui_trans_t *t)
{
    int16_t p;

    if (t == 0) { return 0; }
    p = t->prog.cur;

    switch ((trans_dir_t)t->dir)
    {
        case TRANS_LEFT:  return (int16_t)(-p);              /* 旧页左移 */
        case TRANS_RIGHT: return (int16_t)(p);               /* 旧页右移 */
        case TRANS_UP:    return (int16_t)(-(p / 3));        /* 旧页轻微上移 */
        default:          return 0;                          /* FADE: 不动 */
    }
}

int16_t Anim_TransNewX(const ui_trans_t *t)
{
    int16_t p;

    if (t == 0) { return 0; }
    p = t->prog.cur;

    switch ((trans_dir_t)t->dir)
    {
        case TRANS_LEFT:  return (int16_t)(240 - p);         /* 新页从右进 */
        case TRANS_RIGHT: return (int16_t)(p - 240);         /* 新页从左进 */
        case TRANS_UP:    return (int16_t)(135 - (p % 136)); /* 新页上滑入 */
        default:          return 0;                          /* FADE: 不动 */
    }
}

int16_t Anim_TransOldAlpha(const ui_trans_t *t)
{
    if (t == 0) { return 256; }
    return (int16_t)(256 - t->prog.cur);
}

int16_t Anim_TransNewAlpha(const ui_trans_t *t)
{
    if (t == 0) { return 256; }
    return t->prog.cur;
}

/*==========================================================================
 * 4. 抖动淡出
 *
 * 4x4 Bayer 阈值矩阵，16 级密度。判断"该像素是否保留"。
 * 矩阵值 0..15，dens 0..16：dens > 阈值 则保留。
 *========================================================================*/
static const uint8_t s_bayer4[4][4] =
{
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

bool Anim_DitherKeep(int16_t x, int16_t y, uint8_t dens)
{
    uint8_t th;

    if (dens == 0u)  { return false; }
    if (dens >= 16u) { return true;  }

    /* 取正坐标的模（负坐标也能正常工作） */
    th = s_bayer4[((uint16_t)y) & 3u][((uint16_t)x) & 3u];
    return (dens > th);
}

void Anim_FadeRect(int16_t x, int16_t y, int16_t w, int16_t h,
                   uint16_t bg, uint8_t dens)
{
    int16_t i, j;

    if ((w <= 0) || (h <= 0)) { return; }
    if (dens >= 16u) { return; }        /* 全保留，什么都不用刷 */

    for (j = 0; j < h; j++)
    {
        for (i = 0; i < w; i++)
        {
            if (!Anim_DitherKeep((int16_t)(x + i), (int16_t)(y + j), dens))
            {
                /* 该点应消失：刷成背景色。用横向游程合并，减少调用次数 */
                int16_t run = 1;
                while ((i + run < w) &&
                       (!Anim_DitherKeep((int16_t)(x + i + run), (int16_t)(y + j), dens)))
                {
                    run++;
                }
                TFT_FillRect((int16_t)(x + i), (int16_t)(y + j), run, 1, bg);
                i = (int16_t)(i + run - 1);
            }
        }
    }
}

/*==========================================================================
 * 5. 呼吸 / 脉冲
 *========================================================================*/
void Pulse_Start(pulse_t *p, int16_t lo, int16_t hi, uint16_t period_ms)
{
    if (p == 0) { return; }
    p->lo        = lo;
    p->hi        = hi;
    p->period_ms = (period_ms == 0u) ? 1u : period_ms;
    p->el_ms     = 0u;
    p->active    = 1u;
}

void Pulse_Stop(pulse_t *p)
{
    if (p == 0) { return; }
    p->active = 0u;
    p->el_ms  = 0u;
}

bool Pulse_Step(pulse_t *p, uint16_t dt_ms)
{
    if ((p == 0) || (p->active == 0u)) { return false; }

    p->el_ms = (uint16_t)(p->el_ms + dt_ms);
    if (p->el_ms >= p->period_ms)
    {
        p->el_ms = (uint16_t)(p->el_ms % p->period_ms);
    }
    return true;
}

int16_t Pulse_Value(const pulse_t *p)
{
    int16_t  t;
    int16_t  p256;

    if ((p == 0) || (p->active == 0u)) { return 0; }

    /* 相位 0..256（半周期用来做三角波，全周期做正弦近似） */
    t = (int16_t)(((int32_t)p->el_ms * 256) / (int32_t)p->period_ms);

    /* 三角波：0->256->0，比 sin 省，视觉上一样"呼吸" */
    if (t < 128)
    {
        p256 = (int16_t)(t * 2);
    }
    else
    {
        p256 = (int16_t)((256 - t) * 2);
    }

    /* 用 ease-out 让呼吸两端略缓，更自然 */
    p256 = Anim_Ease(EASE_INOUT, p256);

    return (int16_t)(p->lo + mul256((int16_t)(p->hi - p->lo), p256));
}
