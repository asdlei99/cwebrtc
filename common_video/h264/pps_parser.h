/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef COMMON_VIDEO_H264_PPS_PARSER_H_
#define COMMON_VIDEO_H264_PPS_PARSER_H_

#include "absl/types/optional.h"

namespace rtc {
class BitBuffer;
}

namespace webrtc {

// A class for parsing out picture parameter set (PPS) data from a H264 NALU.
class PpsParser {
 public:
  // The parsed state of the PPS. Only some select values are stored.
  // Add more as they are actually needed.
  struct PpsState {
    PpsState() = default;
    /*
    ��ʶλ�����ڱ�ʾ��������ͷ�е������﷨Ԫ�� 
    delta_pic_order_cnt_bottom �� delta_pic_order_cn �Ƿ���ڵı�ʶ��
    �������﷨Ԫ�ر�ʾ��ĳһ֡�ĵ׳��� POC �ļ��㷽����
    */
    bool bottom_field_pic_order_in_frame_present_flag = false;
    //weighted_pred_flag ���� 0 ��ʾ��Ȩ��Ԥ�ⲻӦ���� P �� SP ������
    //weighted_pred_flag ���� 1 ��ʾ�� P �� SP ������Ӧʹ�ü�Ȩ��Ԥ�⡣
    bool weighted_pred_flag = false;
    /*
    entropy_coding_mode_flag ����ѡȡ�﷨Ԫ�ص��ر��뷽ʽ�����﷨������������ʶ�������������£�

        ��� entropy_coding_mode_flag ���� 0����ô�����﷨������ߵ���������ָ���ķ���
        ����entropy_coding_mode_flag ���� 1�����Ͳ����﷨�����ұߵ���������ָ���ķ���

    ���磺��һ������﷨Ԫ���У�������� mb_type ���﷨Ԫ��������Ϊ ��ue (v) | ae (v)����
        �� baseline profile �������²���ָ�����ײ����룬
        �� main profile �������²��� CABAC ���롣
    */
    bool entropy_coding_mode_flag = false;
    /*
    weighted_bipred_idc ��ֵӦ���� 0 �� 2 ֮�䣨���� 0 �� 2��:

        weighted_bipred_idc ���� 0 ��ʾ B ����Ӧ�ò���Ĭ�ϵļ�ȨԤ�⡣
        weighted_bipred_idc ���� 1 ��ʾ B ����Ӧ�ò��þ���ָ���ļ�ȨԤ�⡣
        weighted_bipred_idc ���� 2 ��ʾ B ����Ӧ�ò��������ļ�ȨԤ�⡣
    */
    uint32_t weighted_bipred_idc = false;
    uint32_t redundant_pic_cnt_present_flag = 0;
    // pic_init_qp_minus26 ��ʾÿ�������� SliceQPY ��ʼֵ�� 26��
    // ������� 0 ֵ�� slice_qp_delta ʱ���ó�ʼֵ�������㱻������
    // �����ں������� 0 ֵ�� mb_qp_delta ʱ��һ����������
    //pic_init_qp_minus26 ��ֵӦ���� -(26 + QpBdOffsetY) �� +25 ֮�䣨�����߽�ֵ��
    int pic_init_qp_minus26 = 0; 
    uint32_t id = 0;
    uint32_t sps_id = 0;

    /*
    
    ��ѡ��������
        slice_group_map_type
        slice_group_map_type ��ʾ��������������ӳ�䵥Ԫ��ӳ������α���ġ�lice_group_map_type ��ȡֵ��ΧӦ���� 0 �� 6 �ڣ����� 0 �� 6����

        slice_group_map_type ���� 0 ��ʾ����ɨ��������顣
        slice_group_map_type ���� 1 ��ʾһ�ַ�ɢ��������ӳ�䡣
        slice_group_map_type ���� 2 ��ʾһ������ǰ���������һ�����������顣
        slice_group_map_type ��ֵ���� 3��4 �� 5 ��ʾ�任�������顣�� num_slice_groups_minus1 ������ 1 ʱ��slice_group_map_type ��Ӧ���� 3��4 �� 5��
        slice_group_map_type ���� 6 ��ʾ��ÿ��������ӳ�䵥Ԫ����ط���һ�������顣������ӳ�䵥Ԫ�涨���£�

        ��� frame_mbs_only_flag ���� 0��mb_adaptive_frame_field_flag ���� 1�����ұ���ͼ����һ��֡����ô������ӳ�䵥Ԫ���Ǻ��Ե�Ԫ��
        ������� frame_mbs_only_flag ���� 1 ����һ������ͼ����һ������������ӳ�䵥Ԫ���Ǻ��ĵ�Ԫ��
        ����frame_mbs_only_flag ���� 0�� mb_adaptive_frame_field_flag ���� 0�����ұ���ͼ����һ��֡����������ӳ�䵥Ԫ��������һ�� MBAFF ֡�е�һ��֡������һ����ֱ���ڵ��������ĵ�Ԫ��
        run_length_minus1
        run_length_minus1 [i] ����ָ��������ӳ�䵥Ԫ�Ĺ�դɨ��˳���з������ i �������������������ӳ�䵥Ԫ����Ŀ��run_length_minus1 [i] ��ȡֵ��ΧӦ���� 0 �� PicSizeInMapUnits �C 1 �ڣ������߽�ֵ����
    */
  };

  // Unpack RBSP and parse PPS state from the supplied buffer.
  static absl::optional<PpsState> ParsePps(const uint8_t* data, size_t length);

  static bool ParsePpsIds(const uint8_t* data,
                          size_t length,
                          uint32_t* pps_id,
                          uint32_t* sps_id);

  static absl::optional<uint32_t> ParsePpsIdFromSlice(const uint8_t* data,
                                                      size_t length);

 protected:
  // Parse the PPS state, for a bit buffer where RBSP decoding has already been
  // performed.
  static absl::optional<PpsState> ParseInternal(rtc::BitBuffer* bit_buffer);
  static bool ParsePpsIdsInternal(rtc::BitBuffer* bit_buffer,
                                  uint32_t* pps_id,
                                  uint32_t* sps_id);
};

}  // namespace webrtc

#endif  // COMMON_VIDEO_H264_PPS_PARSER_H_
