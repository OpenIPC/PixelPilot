package com.geehe.wfbngrtl8812;

import androidx.annotation.Keep;

@Keep
public class WfbNGStats {
    public final int count_p_all;
    public final int count_p_dec_err;
    public final int count_p_dec_ok;
    public final int count_p_fec_recovered;
    public final int count_p_lost;
    public final int count_p_bad;
    public final int count_p_override;
    public final int count_p_outgoing;

    public WfbNGStats(int cntPall, int cntDecErr, int cntDecOk, int cntFecRec,
                      int cntLost, int cntBad, int cntOverride, int cntOutgoing) {
        count_p_all = cntPall;
        count_p_dec_err = cntDecErr;
        count_p_dec_ok = cntDecOk;
        count_p_fec_recovered = cntFecRec;
        count_p_lost = cntLost;
        count_p_bad = cntBad;
        count_p_override = cntOverride;
        count_p_outgoing = cntOutgoing;
    }
}
