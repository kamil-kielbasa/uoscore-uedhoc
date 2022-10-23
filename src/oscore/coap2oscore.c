/*
   Copyright (c) 2021 Fraunhofer AISEC. See the COPYRIGHT
   file at the top-level directory of this distribution.

   Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
   http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
   <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
   option. This file may not be copied, modified, or distributed
   except according to those terms.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oscore.h"

#include "oscore/aad.h"
#include "oscore/oscore_coap.h"
#include "oscore/nonce.h"
#include "oscore/option.h"
#include "oscore/oscore_cose.h"
#include "oscore/security_context.h"

#include "common/byte_array.h"
#include "common/oscore_edhoc_error.h"
#include "common/memcpy_s.h"
#include "common/print_util.h"
#include "common/unit_test.h"

/**
 * @brief Extract input CoAP options into E(encrypted) and U(unprotected)
 * @param in_o_coap: input CoAP packet
 * @param e_options: output pointer to E-options
 * @param e_options_cnt: count number of output E-options
 * @param e_options_len: Byte string length of all E-options, which will be used when forming E-options into plaintext
 * @param U_options: output pointer to U-options
 * @param U_options_cnt: count number of output U-options
 * @return err
 *
 */
STATIC enum err inner_outer_option_split(struct o_coap_packet *in_o_coap,
					 struct o_coap_option *e_options,
					 uint8_t *e_options_cnt,
					 uint16_t *e_options_len,
					 struct o_coap_option *U_options,
					 uint8_t *U_options_cnt)
{
	enum err r = ok;

	/* Initialize to 0 */
	*e_options_len = 0;

	uint8_t temp_option_nr = 0;
	uint8_t temp_len = 0;
	uint8_t temp_E_option_delta_sum = 0;
	uint8_t temp_U_option_delta_sum = 0;
	uint8_t delta_extra_bytes = 0;
	uint8_t len_extra_bytes = 0;

	if (MAX_OPTION_COUNT < in_o_coap->options_cnt) {
		return not_valid_input_packet;
	}

	for (uint8_t i = 0; i < in_o_coap->options_cnt; i++) {
		delta_extra_bytes = 0;
		len_extra_bytes = 0;

		temp_option_nr =
			(uint8_t)(temp_option_nr + in_o_coap->options[i].delta);
		temp_len = in_o_coap->options[i].len;

		/* Calculate extra byte length of option delta and option length */
		if (in_o_coap->options[i].delta > 13 &&
		    in_o_coap->options[i].delta < 243)
			delta_extra_bytes = 1;
		else if (in_o_coap->options[i].delta >= 243)
			delta_extra_bytes = 2;
		if (in_o_coap->options[i].len > 13 &&
		    in_o_coap->options[i].len < 243)
			len_extra_bytes = 1;
		else if (in_o_coap->options[i].len >= 243)
			len_extra_bytes = 2;

		/* process special options, see 4.1.3 in RFC8613*/
		/* if the option does not need special processing just put it in the 
		E or U array*/

		switch (temp_option_nr) {
		case OBSERVE:
			/*An observe option in an a CoAP packet is transformed to an inner
			and outer option in a OSCORE packet.*/

			/*
			* Inner option has value NULL if notification or the original value 
			* in the coap packet if registration/cancellation.
			*/
			e_options[*e_options_cnt].delta =
				(uint16_t)(temp_option_nr -
					   temp_E_option_delta_sum);
			if (is_request(in_o_coap)) {
				/*registrations/cancellations are requests */
				e_options[*e_options_cnt].len = temp_len;
				e_options[*e_options_cnt].value =
					in_o_coap->options[i].value;

				/* Add option header length and value length */
				(*e_options_len) =
					(uint16_t)((*e_options_len) + 1 +
						   delta_extra_bytes +
						   len_extra_bytes + temp_len);
			} else {
				/*notifications are responses*/
				e_options[*e_options_cnt].len = 0;
				e_options[*e_options_cnt].value = NULL;

				/* since the option value has length 0, we add 1 for the option header which is always there */
				(*e_options_len)++;
			}

			e_options[*e_options_cnt].option_number =
				temp_option_nr;

			/* Update delta sum of E-options */
			temp_E_option_delta_sum =
				(uint8_t)(temp_E_option_delta_sum +
					  e_options[*e_options_cnt].delta);

			/* Increment E-options count */
			(*e_options_cnt)++;

			/*
			*outer option (value as in the original coap packet
			*/
			U_options[*U_options_cnt].delta =
				(uint16_t)(temp_option_nr -
					   temp_U_option_delta_sum);
			U_options[*U_options_cnt].len = temp_len;
			U_options[*U_options_cnt].value =
				in_o_coap->options[i].value;
			U_options[*U_options_cnt].option_number =
				temp_option_nr;

			/* Update delta sum of E-options */
			temp_U_option_delta_sum =
				(uint8_t)(temp_U_option_delta_sum +
					  U_options[*U_options_cnt].delta);

			/* Increment E-options count */
			(*U_options_cnt)++;

			break;

		default:
			/* check delta, whether current option U or E */
			if (is_class_e(temp_option_nr) == 1) {
				/* E-options, which will be copied in plaintext to be encrypted*/
				e_options[*e_options_cnt].delta =
					(uint16_t)(temp_option_nr -
						   temp_E_option_delta_sum);
				e_options[*e_options_cnt].len = temp_len;
				e_options[*e_options_cnt].value =
					in_o_coap->options[i].value;
				e_options[*e_options_cnt].option_number =
					temp_option_nr;

				/* Update delta sum of E-options */
				temp_E_option_delta_sum =
					(uint8_t)(temp_E_option_delta_sum +
						  e_options[*e_options_cnt]
							  .delta);

				/* Increment E-options count */
				(*e_options_cnt)++;
				/* Add option header length and value length */
				(*e_options_len) =
					(uint16_t)((*e_options_len) + 1 +
						   delta_extra_bytes +
						   len_extra_bytes + temp_len);
			} else {
				/* U-options */
				U_options[*U_options_cnt].delta =
					(uint16_t)(temp_option_nr -
						   temp_U_option_delta_sum);
				U_options[*U_options_cnt].len = temp_len;
				U_options[*U_options_cnt].value =
					in_o_coap->options[i].value;
				U_options[*U_options_cnt].option_number =
					temp_option_nr;

				/* Update delta sum of E-options */
				temp_U_option_delta_sum =
					(uint8_t)(temp_U_option_delta_sum +
						  U_options[*U_options_cnt]
							  .delta);

				/* Increment E-options count */
				(*U_options_cnt)++;
			}
			break;
		}
	}
	return r;
}

/**
 * @brief Build up plaintext which should be encrypted and protected
 * @param in_o_coap: input CoAP packet that will be analyzed
 * @param E_options: E-options, which should be protected
 * @param E_options_cnt: count number of E-options
 * @param plaintext: output plaintext, which will be encrypted
 * @return err
 *
 */
static inline enum err plaintext_setup(struct o_coap_packet *in_o_coap,
				       struct o_coap_option *E_options,
				       uint8_t E_options_cnt,
				       struct byte_array *plaintext)
{
	uint8_t *temp_plaintext_ptr = plaintext->ptr;

	/* Add code to plaintext */
	*temp_plaintext_ptr = in_o_coap->header.code;

	/* Calculate the maximal length of all options, i.e. all options 
	have two bytes extra delta and length */
	uint16_t e_opt_serial_len = 0;
	for (uint8_t i = 0; i < E_options_cnt; i++) {
		e_opt_serial_len = (uint16_t)(e_opt_serial_len + 1 + 2 + 2 +
					      E_options[i].len);
	}
	/* Setup buffer */
	BYTE_ARRAY_NEW(e_opt_serial, E_OPTIONS_BUFF_MAX_LEN, e_opt_serial_len);

	/* Convert all E-options structure to byte string, and copy it to 
	output*/
	TRY(options_into_byte_string(E_options, E_options_cnt, &e_opt_serial));

	uint32_t dest_size = (plaintext->len - (uint32_t)(temp_plaintext_ptr +
							  1 - plaintext->ptr));
	TRY(_memcpy_s(++temp_plaintext_ptr, dest_size, e_opt_serial.ptr,
		      e_opt_serial.len));
	temp_plaintext_ptr += e_opt_serial.len;

	/* Add payload to plaintext*/
	if (in_o_coap->payload_len != 0) {
		/* An extra byte 0xFF before payload*/
		*temp_plaintext_ptr = 0xff;

		dest_size = (plaintext->len - (uint32_t)(temp_plaintext_ptr +
							 1 - plaintext->ptr));
		TRY(_memcpy_s(++temp_plaintext_ptr, dest_size,
			      in_o_coap->payload, in_o_coap->payload_len));
	}
	PRINT_ARRAY("Plain text", plaintext->ptr, plaintext->len);
	return ok;
}

/**
 * @brief   Encrypt incoming plaintext
 * @param   c OSCORE context
 * @param   in_o_coap: input CoAP packet, which will be used to calculate AAD
 *          (additional authentication data)
 * @param   in_plaintext: input plaintext that will be encrypted
 * @param   out_ciphertext: output ciphertext, which contains the encrypted data
 * @return  err
 *
 */
// TODO use byte_array for out_ciphertext
static inline enum err plaintext_encrypt(struct context *c,
					 struct byte_array *aad,
					 struct byte_array *in_plaintext,
					 uint8_t *out_ciphertext,
					 uint32_t out_ciphertext_len)
{
	return oscore_cose_encrypt(in_plaintext, out_ciphertext,
				   out_ciphertext_len, &c->rrc.nonce, aad,
				   &c->sc.sender_key);
}

/**
 * @brief   OSCORE option value length
 * @param   piv set to the sender sequence number in requests or NULL in 
 *          responses
 * @param   kid set to Sender ID in requests or NULL in responses
 * @param   kid_context set to ID context in request when present. If not present or a response set to NULL
 * @return  length of the OSCORE option value
 */
static inline uint8_t get_oscore_opt_val_len(struct byte_array *piv,
					     struct byte_array *kid,
					     struct byte_array *kid_context)
{
	uint8_t l;
	l = (uint8_t)(piv->len + kid_context->len + kid->len);
	if (l) {
		/*if any of piv, kit_context or kit is present 1 byte for the flags is reserved */
		l++;
	}
	if (kid_context->len) {
		/*if kit_context is present one byte is reserved for the s field*/
		l++;
	}
	return l;
}

/**
 * @brief   Generate an OSCORE option. The oscore option value length must 
 *          be calculated before this function is called and set in 
 *          oscore_option.len. In addition oscore_option.val pointer should
 *          be set to a buffer with length oscore_option.len. 
 * @param   piv set to the trimmed sender sequence number in requests or NULL 
 *          in responses
 * @param   kid set to Sender ID in requests or NULL in responses
 * @param   kid_context set to ID context in request when present. If not 
 *          present or a response set to NULL
 * @param   oscore_option: output pointer OSCORE option structure
 * @return  err
 */
static inline enum err
oscore_option_generate(struct byte_array *piv, struct byte_array *kid,
		       struct byte_array *kid_context,
		       struct oscore_option *oscore_option)
{
	uint32_t dest_size;
	oscore_option->option_number = OSCORE;

	if (oscore_option->len == 0) {
		oscore_option->value = NULL;
	} else {
		memset(oscore_option->value, 0, oscore_option->len);

		uint8_t *temp_ptr = oscore_option->value;

		if (piv->len != 0) {
			/* Set header bits of PIV */
			oscore_option->value[0] =
				(uint8_t)(oscore_option->value[0] | piv->len);
			/* copy PIV (sender sequence) */

			dest_size = (uint32_t)(oscore_option->len -
					       (temp_ptr + 1 -
						oscore_option->value));
			TRY(_memcpy_s(++temp_ptr, dest_size, piv->ptr,
				      piv->len));

			temp_ptr += piv->len;
		}

		if (kid_context->len != 0) {
			/* Set header flag bit of KID context */
			oscore_option->value[0] |= COMP_OSCORE_OPT_KIDC_H_MASK;
			/* Copy length and context value */
			*temp_ptr = (uint8_t)(kid_context->len);

			dest_size = (uint32_t)(oscore_option->len -
					       (temp_ptr + 1 -
						oscore_option->value));
			TRY(_memcpy_s(++temp_ptr, dest_size, kid_context->ptr,
				      kid_context->len));

			temp_ptr += kid_context->len;
		}

		/* Set header flag bit of KID */
		/* The KID header flag is set always in requests */
		/* This function is not called in responses */
		oscore_option->value[0] |= COMP_OSCORE_OPT_KID_K_MASK;
		if (kid->len != 0) {
			/* Copy KID */
			dest_size =
				(uint32_t)(oscore_option->len -
					   (temp_ptr - oscore_option->value));
			TRY(_memcpy_s(temp_ptr, dest_size, kid->ptr, kid->len));
		}
	}

	PRINT_ARRAY("OSCORE option value", oscore_option->value,
		    oscore_option->len);
	return ok;
}

/**
 * @brief Generate an OSCORE packet with all needed data
 * @param in_o_coap: input CoAP packet
 * @param out_oscore: output pointer to OSCORE packet
 * @param U_options: pointer to array of all unprotected options, including OSCORE_option
 * @param U_options_cnt: count number of U-options
 * @param in_ciphertext: input ciphertext, will be set into payload in OSCORE packet
 * @param oscore_option: The OSCORE option
 * @return err
 *
 */
STATIC enum err oscore_pkg_generate(struct o_coap_packet *in_o_coap,
				    struct o_coap_packet *out_oscore,
				    struct o_coap_option *u_options,
				    uint8_t u_options_cnt,
				    uint8_t *in_ciphertext,
				    uint32_t in_ciphertext_len,
				    struct oscore_option *oscore_option)
{
	/* Set OSCORE header and Token*/
	out_oscore->header.ver = in_o_coap->header.ver;
	out_oscore->header.type = in_o_coap->header.type;
	out_oscore->header.TKL = in_o_coap->header.TKL;
	out_oscore->header.MID = in_o_coap->header.MID;
	if (out_oscore->header.TKL == 0) {
		out_oscore->token = NULL;
	} else {
		out_oscore->token = in_o_coap->token;
	}

	bool observe = is_observe(u_options, u_options_cnt);
	if (is_request(in_o_coap)) {
		if (observe) {
			out_oscore->header.code = CODE_REQ_FETCH;
		} else {
			out_oscore->header.code = CODE_REQ_POST;
		}
	} else {
		if (observe) {
			out_oscore->header.code = CODE_RESP_CONTENT;
		} else {
			out_oscore->header.code = CODE_RESP_CHANGED;
		}
	}

	/* U-options + OSCORE option (compare oscore option number with others)
	 Find out the appropriate position of OSCORE option */
	uint8_t oscore_opt_pos = u_options_cnt;
	for (uint8_t i = 0; i < u_options_cnt; i++) {
		/* Once found, finish the for-loop */
		if (u_options[i].option_number > OSCORE) {
			oscore_opt_pos = i;
			break;
		}
	}

	/* Update options count number to output*/
	out_oscore->options_cnt = (uint8_t)(1 + u_options_cnt);

	uint8_t temp_opt_number_sum = 0;
	/* Show the position of U-options */
	uint8_t u_opt_pos = 0;
	for (uint8_t i = 0; i < u_options_cnt + 1; i++) {
		if (i == oscore_opt_pos) {
			/* OSCORE_option */
			out_oscore->options[i].delta =
				(uint16_t)(oscore_option->option_number -
					   temp_opt_number_sum);
			out_oscore->options[i].len = oscore_option->len;
			out_oscore->options[i].option_number =
				oscore_option->option_number;
			out_oscore->options[i].value = oscore_option->value;
		} else {
			/* U-options */
			out_oscore->options[i].delta =
				(uint16_t)(u_options[u_opt_pos].option_number -
					   temp_opt_number_sum);
			out_oscore->options[i].len = u_options[u_opt_pos].len;
			out_oscore->options[i].option_number =
				u_options[u_opt_pos].option_number;
			out_oscore->options[i].value =
				u_options[u_opt_pos].value;

			u_opt_pos++;
		}
		temp_opt_number_sum = (uint8_t)(temp_opt_number_sum +
						out_oscore->options[i].delta);
	}

	/* Protected Payload */
	out_oscore->payload_len = in_ciphertext_len;
	out_oscore->payload = in_ciphertext;
	return ok;
}

/**
 *@brief 	Converts a CoAP packet to OSCORE packet
 *@note		For messaging layer packets (simple ACK with no payload, code 0.00),
 *			encryption is dismissed and raw input buffer is copied, 
 *			as specified at section 4.2 in RFC8613.
 *@param	buf_o_coap a buffer containing a CoAP packet
 *@param	buf_o_coap_len length of the CoAP buffer
 *@param	buf_oscore a buffer where the OSCORE packet will be written
 *@param	buf_oscore_len length of the OSCORE packet
 *@param	c a struct containing the OSCORE context
 *
 *@return	err
 */
enum err coap2oscore(uint8_t *buf_o_coap, uint32_t buf_o_coap_len,
		     uint8_t *buf_oscore, uint32_t *buf_oscore_len,
		     struct context *c)
{
	struct o_coap_packet o_coap_pkt;
	struct byte_array buf;
	uint32_t plaintext_len = 0;

	PRINT_MSG("\n\n\ncoap2oscore***************************************\n");
	PRINT_ARRAY("Input CoAP packet", buf_o_coap, buf_o_coap_len);

	buf.len = buf_o_coap_len;
	buf.ptr = buf_o_coap;

	/*Parse the coap buf into a CoAP struct*/
	memset(&o_coap_pkt, 0, sizeof(o_coap_pkt));
	TRY(buf2coap(&buf, &o_coap_pkt));

	/* Dismiss OSCORE encryption if messaging layer detected (simple ACK, code=0.00) */
	if ((CODE_EMPTY == o_coap_pkt.header.code) &&
	    (TYPE_ACK == o_coap_pkt.header.type)) {
		PRINT_MSG(
			"Messaging Layer CoAP packet detected, encryption dismissed\n");
		*buf_oscore_len = buf_o_coap_len;
		return _memcpy_s(buf_oscore, buf_o_coap_len, buf_o_coap,
				 buf_o_coap_len);
	}

	/* 1. Divide CoAP options into E-option and U-option */
	struct o_coap_option e_options[MAX_OPTION_COUNT];
	uint8_t e_options_cnt = 0;
	uint16_t e_options_len = 0;
	struct o_coap_option u_options[MAX_OPTION_COUNT];
	uint8_t u_options_cnt = 0;

	/* Analyze CoAP options, extract E-options and U-options */
	TRY(inner_outer_option_split(&o_coap_pkt, e_options, &e_options_cnt,
				     &e_options_len, u_options,
				     &u_options_cnt));

	/* 2. Create plaintext (code + E-options + o_coap_payload) */
	/* Calculate complete plaintext length: 1 byte code + E-options + 1 byte 0xFF + payload */
	plaintext_len = (uint32_t)(1 + e_options_len);

	if (o_coap_pkt.payload_len) {
		plaintext_len = plaintext_len + 1 + o_coap_pkt.payload_len;
	}

	/* Setup buffer for plaintext */
	BYTE_ARRAY_NEW(plaintext, MAX_PLAINTEXT_LEN, plaintext_len);

	/* Combine code, E-options and payload of CoAP to plaintext */
	TRY(plaintext_setup(&o_coap_pkt, e_options, e_options_cnt, &plaintext));

	/* Generate OSCORE option */
	struct oscore_option oscore_option;
	oscore_option.option_number = OSCORE;

	/*
    * If the packet is a request or a response with an observe option:
	*	* the OSCORE option has a value 
    *	* nonce needs to be generated
    */
	bool request = is_request(&o_coap_pkt);
	if (request || is_observe(u_options, u_options_cnt) || c->rrc.reboot) {
		BYTE_ARRAY_NEW(piv, MAX_PIV_LEN, MAX_PIV_LEN);
		TRY(sender_seq_num2piv(c->sc.sender_seq_num++, &piv));

		/*in case of request update request_piv and request_kid*/
		TRY(update_request_piv_request_kid(c, &piv, &c->sc.sender_id,
						   request));

		/*in case of first response after reboot save ECHO option*/
		if (c->rrc.reboot) {
			TRY(cache_echo_val(&c->rrc.echo_opt_val, e_options,
					   e_options_cnt));
			c->rrc.reboot = false;
		}

		/*compute nonce*/
		TRY(create_nonce(&c->sc.sender_id, &piv, &c->cc.common_iv,
				 &c->rrc.nonce));

		/*compute the OSCORE option value*/
		oscore_option.len = get_oscore_opt_val_len(
			&piv, &c->sc.sender_id, &c->cc.id_context);
		if (oscore_option.len > OSCORE_OPT_VALUE_LEN) {
			return oscore_valuelen_to_long_error;
		}

		oscore_option.value = oscore_option.buf;
		TRY(oscore_option_generate(&piv, &c->sc.sender_id,
					   &c->cc.id_context, &oscore_option));
	} else {
		oscore_option.len = 0;
		oscore_option.value = NULL;
	}

	BYTE_ARRAY_NEW(aad, MAX_AAD_LEN, MAX_AAD_LEN);
	TRY(create_aad(NULL, 0, c->cc.aead_alg, &c->rrc.request_kid,
		       &c->rrc.request_piv, &aad));

	/*3. Encrypt the created plaintext*/
	uint32_t ciphertext_len = plaintext.len + AUTH_TAG_LEN;
	TRY(check_buffer_size(MAX_CIPHERTEXT_LEN, ciphertext_len));
	uint8_t ciphertext[MAX_CIPHERTEXT_LEN];

	TRY(plaintext_encrypt(c, &aad, &plaintext, (uint8_t *)&ciphertext,
			      ciphertext_len));

	/*create an OSCORE packet*/
	struct o_coap_packet oscore_pkt;
	TRY(oscore_pkg_generate(&o_coap_pkt, &oscore_pkt, u_options,
				u_options_cnt, (uint8_t *)&ciphertext,
				ciphertext_len, &oscore_option));

	/*convert the oscore pkg to byte string*/
	return coap2buf(&oscore_pkt, buf_oscore, buf_oscore_len);
}
