#include <stdint.h>

#define F (1 << 14)  // 고정 소수점 표현을 위한 상수, 여기서 14는 소수 부분의 비트 수
#define INT_MAX ((1 << 31) - 1)
#define INT_MIN (-(1 << 31))

/*
 * 고정 소수점을 위한 산술 연산
 * n: int ; x, y: 고정 소수점 숫자 ; F: 17.14로 표현한 1
 */

int int_to_fp(int n);         /* integer를 fixed point로 전환 */
int fp_to_int_round(int x);   /* FP를 int로 전환(반올림) */
int fp_to_int(int x);         /* FP를 int로 전환(버림) */
int add_fp(int x, int y);     /* FP의 덧셈 */
int add_mixed(int x, int n);  /* FP와 int의 덧셈 */
int sub_fp(int x, int y);     /* FP의 뺄셈(x-y) */
int sub_mixed(int x, int n);  /* FP와 int의 뺄셈(x-n) */
int mult_fp(int x, int y);    /* FP의 곱셈 */
int mult_mixed(int x, int y); /* FP와 int의 곱셈 */
int div_fp(int x, int y);     /* FP의 나눗셈(x/y) */
int div_mixed(int x, int n);  /* FP와 int 나눗셈(x/n) */


/* 함수 본체 */

/* integer를 fixed point로 전환 */
int int_to_fp(int n) {
    return n * F;
}

/* FP를 int로 전환(반올림) */
int fp_to_int_round(int x) {
    if (x >= 0)
        return (x + F / 2) / F;
    else
        return (x - F / 2) / F;
}

/* FP를 int로 전환(버림) */
int fp_to_int(int x) {
    return x / F;
}

/* FP의 덧셈 */
int add_fp(int x, int y) {
    return x + y;
}

/* FP와 int의 덧셈 */
int add_mixed(int x, int n) {
    return x + n * F;
}

/* FP의 뺄셈(x-y) */
int sub_fp(int x, int y) {
    return x - y;
}

/* FP와 int의 뺄셈(x-n) */
int sub_mixed(int x, int n) {
    return x - n * F;
}

/* FP의 곱셈 */
int mult_fp(int x, int y) {
    return ((int64_t)x) * y / F;
}

/* FP와 int의 곱셈 */
int mult_mixed(int x, int n) {
    return x * n;
}

/* FP의 나눗셈(x/y) */
int div_fp(int x, int y) {
    return ((int64_t)x) * F / y;
}

/* FP와 int 나눗셈(x/n) */
int div_mixed(int x, int n) {
    return x / n;
}
