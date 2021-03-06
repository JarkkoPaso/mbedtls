/*
 *  Elliptic curve Diffie-Hellman
 *
 *  Copyright (C) 2006-2015, ARM Limited, All Rights Reserved
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  This file is part of mbed TLS (https://tls.mbed.org)
 */

/*
 * References:
 *
 * SEC1 http://www.secg.org/index.php?action=secg,docs_secg
 * RFC 4492
 */

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_ECDH_C)

#include "mbedtls/ecdh.h"

#include <string.h>

/*
 * Generate public key (restartable version)
 */
static int32_t ecdh_gen_public_restartable( mbedtls_ecp_group *grp,
                    mbedtls_mpi *d, mbedtls_ecp_point *Q,
                    int32_t (*f_rng)(void *, unsigned char *, size_t),
                    void *p_rng,
                    mbedtls_ecp_restart_ctx *rs_ctx )
{
    int32_t ret;

    /* If multiplication is in progress, we already generated a privkey */
#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( rs_ctx == NULL || rs_ctx->rsm == NULL )
#endif
        MBEDTLS_MPI_CHK( mbedtls_ecp_gen_privkey( grp, d, f_rng, p_rng ) );

    MBEDTLS_MPI_CHK( mbedtls_ecp_mul_restartable( grp, Q, d, &grp->G,
                                                  f_rng, p_rng, rs_ctx ) );

cleanup:
    return( ret );
}

/*
 * Generate public key
 */
int32_t mbedtls_ecdh_gen_public( mbedtls_ecp_group *grp, mbedtls_mpi *d, mbedtls_ecp_point *Q,
                     int32_t (*f_rng)(void *, unsigned char *, size_t),
                     void *p_rng )
{
    return( ecdh_gen_public_restartable( grp, d, Q, f_rng, p_rng, NULL ) );
}

/*
 * Compute shared secret (SEC1 3.3.1)
 */
static int32_t ecdh_compute_shared_restartable( mbedtls_ecp_group *grp,
                         mbedtls_mpi *z,
                         const mbedtls_ecp_point *Q, const mbedtls_mpi *d,
                         int32_t (*f_rng)(void *, unsigned char *, size_t),
                         void *p_rng,
                         mbedtls_ecp_restart_ctx *rs_ctx )
{
    int32_t ret;
    mbedtls_ecp_point P;

    mbedtls_ecp_point_init( &P );

    /*
     * Make sure Q is a valid pubkey before using it
     */
    MBEDTLS_MPI_CHK( mbedtls_ecp_check_pubkey( grp, Q ) );

    MBEDTLS_MPI_CHK( mbedtls_ecp_mul_restartable( grp, &P, d, Q,
                                                  f_rng, p_rng, rs_ctx ) );

    if( mbedtls_ecp_is_zero( &P ) )
    {
        ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        goto cleanup;
    }

    MBEDTLS_MPI_CHK( mbedtls_mpi_copy( z, &P.X ) );

cleanup:
    mbedtls_ecp_point_free( &P );

    return( ret );
}

/*
 * Compute shared secret (SEC1 3.3.1)
 */
int32_t mbedtls_ecdh_compute_shared( mbedtls_ecp_group *grp, mbedtls_mpi *z,
                         const mbedtls_ecp_point *Q, const mbedtls_mpi *d,
                         int32_t (*f_rng)(void *, unsigned char *, size_t),
                         void *p_rng )
{
    return( ecdh_compute_shared_restartable( grp, z, Q, d,
                                             f_rng, p_rng, NULL ) );
}

/*
 * Initialize context
 */
void mbedtls_ecdh_init( mbedtls_ecdh_context *ctx )
{
    memset( ctx, 0, sizeof( mbedtls_ecdh_context ) );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    mbedtls_ecp_restart_init( &ctx->rs );
#endif
}

/*
 * Free context
 */
void mbedtls_ecdh_free( mbedtls_ecdh_context *ctx )
{
    if( ctx == NULL )
        return;

    mbedtls_ecp_group_free( &ctx->grp );
    mbedtls_ecp_point_free( &ctx->Q   );
    mbedtls_ecp_point_free( &ctx->Qp  );
    mbedtls_ecp_point_free( &ctx->Vi  );
    mbedtls_ecp_point_free( &ctx->Vf  );
    mbedtls_mpi_free( &ctx->d  );
    mbedtls_mpi_free( &ctx->z  );
    mbedtls_mpi_free( &ctx->_d );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    mbedtls_ecp_restart_free( &ctx->rs );
#endif
}

#if defined(MBEDTLS_ECP_RESTARTABLE)
/*
 * Enable restartable operations for context
 */
void mbedtls_ecdh_enable_restart( mbedtls_ecdh_context *ctx )
{
    ctx->restart_enabled = 1;
}
#endif

/*
 * Setup and write the ServerKeyExhange parameters (RFC 4492)
 *      struct {
 *          ECParameters    curve_params;
 *          ECPoint         public;
 *      } ServerECDHParams;
 */
int32_t mbedtls_ecdh_make_params( mbedtls_ecdh_context *ctx, size_t *olen,
                      unsigned char *buf, size_t blen,
                      int32_t (*f_rng)(void *, unsigned char *, size_t),
                      void *p_rng )
{
    int32_t ret;
    size_t grp_len, pt_len;
    mbedtls_ecp_restart_ctx *rs_ctx = NULL;

    if( ctx == NULL || ctx->grp.pbits == 0 )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( ctx->restart_enabled )
        rs_ctx = &ctx->rs;
#endif

    if( ( ret = ecdh_gen_public_restartable( &ctx->grp, &ctx->d, &ctx->Q,
                    f_rng, p_rng, rs_ctx ) ) != 0 )
        return( ret );

    if( ( ret = mbedtls_ecp_tls_write_group( &ctx->grp, &grp_len, buf, blen ) )
                != 0 )
        return( ret );

    buf += grp_len;
    blen -= grp_len;

    if( ( ret = mbedtls_ecp_tls_write_point( &ctx->grp, &ctx->Q, ctx->point_format,
                                     &pt_len, buf, blen ) ) != 0 )
        return( ret );

    *olen = grp_len + pt_len;
    return( 0 );
}

/*
 * Read the ServerKeyExhange parameters (RFC 4492)
 *      struct {
 *          ECParameters    curve_params;
 *          ECPoint         public;
 *      } ServerECDHParams;
 */
int32_t mbedtls_ecdh_read_params( mbedtls_ecdh_context *ctx,
                      const unsigned char **buf, const unsigned char *end )
{
    int32_t ret;

    if( ( ret = mbedtls_ecp_tls_read_group( &ctx->grp, buf, end - *buf ) ) != 0 )
        return( ret );

    if( ( ret = mbedtls_ecp_tls_read_point( &ctx->grp, &ctx->Qp, buf, end - *buf ) )
                != 0 )
        return( ret );

    return( 0 );
}

/*
 * Get parameters from a keypair
 */
int32_t mbedtls_ecdh_get_params( mbedtls_ecdh_context *ctx, const mbedtls_ecp_keypair *key,
                     mbedtls_ecdh_side side )
{
    int32_t ret;

    if( ( ret = mbedtls_ecp_group_copy( &ctx->grp, &key->grp ) ) != 0 )
        return( ret );

    /* If it's not our key, just import the public part as Qp */
    if( side == MBEDTLS_ECDH_THEIRS )
        return( mbedtls_ecp_copy( &ctx->Qp, &key->Q ) );

    /* Our key: import public (as Q) and private parts */
    if( side != MBEDTLS_ECDH_OURS )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    if( ( ret = mbedtls_ecp_copy( &ctx->Q, &key->Q ) ) != 0 ||
        ( ret = mbedtls_mpi_copy( &ctx->d, &key->d ) ) != 0 )
        return( ret );

    return( 0 );
}

/*
 * Setup and export the client public value
 */
int32_t mbedtls_ecdh_make_public( mbedtls_ecdh_context *ctx, size_t *olen,
                      unsigned char *buf, size_t blen,
                      int32_t (*f_rng)(void *, unsigned char *, size_t),
                      void *p_rng )
{
    int32_t ret;
    mbedtls_ecp_restart_ctx *rs_ctx = NULL;

    if( ctx == NULL || ctx->grp.pbits == 0 )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( ctx->restart_enabled )
        rs_ctx = &ctx->rs;
#endif

    if( ( ret = ecdh_gen_public_restartable( &ctx->grp, &ctx->d, &ctx->Q,
                    f_rng, p_rng, rs_ctx ) ) != 0 )
        return( ret );

    return mbedtls_ecp_tls_write_point( &ctx->grp, &ctx->Q, ctx->point_format,
                                olen, buf, blen );
}

/*
 * Parse and import the client's public value
 */
int32_t mbedtls_ecdh_read_public( mbedtls_ecdh_context *ctx,
                      const unsigned char *buf, size_t blen )
{
    int32_t ret;
    const unsigned char *p = buf;

    if( ctx == NULL )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    if( ( ret = mbedtls_ecp_tls_read_point( &ctx->grp, &ctx->Qp, &p, blen ) ) != 0 )
        return( ret );

    if( (size_t)( p - buf ) != blen )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    return( 0 );
}

/*
 * Derive and export the shared secret
 */
int32_t mbedtls_ecdh_calc_secret( mbedtls_ecdh_context *ctx, size_t *olen,
                      unsigned char *buf, size_t blen,
                      int32_t (*f_rng)(void *, unsigned char *, size_t),
                      void *p_rng )
{
    int32_t ret;
    mbedtls_ecp_restart_ctx *rs_ctx = NULL;

    if( ctx == NULL || ctx->grp.pbits == 0 )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

#if defined(MBEDTLS_ECP_RESTARTABLE)
    if( ctx->restart_enabled )
        rs_ctx = &ctx->rs;
#endif

    if( ( ret = ecdh_compute_shared_restartable( &ctx->grp,
                    &ctx->z, &ctx->Qp, &ctx->d, f_rng, p_rng, rs_ctx ) ) != 0 )
    {
        return( ret );
    }

    if( mbedtls_mpi_size( &ctx->z ) > blen )
        return( MBEDTLS_ERR_ECP_BAD_INPUT_DATA );

    *olen = ctx->grp.pbits / 8 + ( ( ctx->grp.pbits % 8 ) != 0 );
    return mbedtls_mpi_write_binary( &ctx->z, buf, *olen );
}

#endif /* MBEDTLS_ECDH_C */
