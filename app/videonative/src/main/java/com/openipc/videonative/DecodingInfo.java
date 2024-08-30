package com.openipc.videonative;

import androidx.annotation.Keep;

import java.util.LinkedHashMap;
import java.util.Map;

@Keep
public class DecodingInfo {
    public final float currentFPS;
    public final float currentKiloBitsPerSecond;
    public final float avgParsingTime_ms;
    public final float avgWaitForInputBTime_ms;
    public final float avgHWDecodingTime_ms; //time the hw decoder was holding on to frames. Not the full decoding time !
    public final float avgTotalDecodingTime_ms;
    public final int nNALU;
    public final int nNALUSFeeded;
    public final int nDecodedFrames;
    public final int nCodec;

    public DecodingInfo() {
        currentFPS = 0;
        currentKiloBitsPerSecond = 0;
        avgParsingTime_ms = 0;
        avgWaitForInputBTime_ms = 0;
        avgHWDecodingTime_ms = 0;
        nNALU = 0;
        nNALUSFeeded = 0;
        avgTotalDecodingTime_ms = 0;
        nDecodedFrames = 0;
        nCodec = 0;
    }

    public DecodingInfo(float currentFPS, float currentKiloBitsPerSecond, float avgParsingTime_ms,
                        float avgWaitForInputBTime_ms, float avgHWDecodingTime_ms,
                        int nNALU, int nNALUSFeeded, int nDecodedFrames, int nCodec) {
        this.currentFPS = currentFPS;
        this.currentKiloBitsPerSecond = currentKiloBitsPerSecond;
        this.avgParsingTime_ms = avgParsingTime_ms;
        this.avgWaitForInputBTime_ms = avgWaitForInputBTime_ms;
        this.avgHWDecodingTime_ms = avgHWDecodingTime_ms;
        this.avgTotalDecodingTime_ms = avgParsingTime_ms + avgWaitForInputBTime_ms + avgHWDecodingTime_ms;
        this.nNALU = nNALU;
        this.nNALUSFeeded = nNALUSFeeded;
        this.nDecodedFrames = nDecodedFrames;
        this.nCodec = nCodec;
    }

    public LinkedHashMap<String, Object> toMap() {
        LinkedHashMap<String, Object> decodingInfo = new LinkedHashMap<>();
        decodingInfo.put("avgTotalDecodingTime_ms", avgTotalDecodingTime_ms);
        decodingInfo.put("avgParsingTime_ms", avgParsingTime_ms);
        decodingInfo.put("avgWaitForInputBTime_ms", avgWaitForInputBTime_ms);
        decodingInfo.put("avgHWDecodingTime_ms", avgHWDecodingTime_ms);
        decodingInfo.put("currentFPS", currentFPS);
        decodingInfo.put("currentKiloBitsPerSecond", currentKiloBitsPerSecond);
        decodingInfo.put("nNALU", nNALU);
        decodingInfo.put("nNALUSFeeded", nNALUSFeeded);
        decodingInfo.put("nDecodedFrames", nDecodedFrames);
        decodingInfo.put("nCodec", nCodec);
        return decodingInfo;
    }

    public String toString(final boolean newline) {
        final StringBuilder builder = new StringBuilder();
        builder.append("Decoding info:\n");
        final Map<String, Object> map = toMap();
        for (final String key : map.keySet()) {
            //Either float or int, toString available
            final Object value = map.get(key);
            builder.append(key).append(":").append(value);
            if (newline) builder.append("\n");
        }
        return builder.toString();
    }

    @Override
    public String toString() {
        return toString(false);
    }
}
