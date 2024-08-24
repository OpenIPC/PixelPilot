package com.geehe.fpvue.osd;

import static android.content.Context.MODE_PRIVATE;
import static java.lang.Math.atan2;
import static java.lang.Math.cos;
import static java.lang.Math.floor;
import static java.lang.Math.sin;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.CountDownTimer;
import android.os.Handler;
import android.view.View;

import com.geehe.fpvue.R;
import com.geehe.fpvue.databinding.ActivityVideoBinding;
import com.geehe.mavlink.MavlinkData;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

public class OSDManager {
    private static final String TAG = "com.geehe.fpvue.osd";
    private final ActivityVideoBinding binding;
    private final Context context;
    private final Handler handler = new Handler();
    private final byte FLIGHT_MODE_MANUAL = 1, FLIGHT_MODE_STAB = 2, FLIGHT_MODE_ALTH = 3, FLIGHT_MODE_LOITER = 4, FLIGHT_MODE_RTL = 5, FLIGHT_MODE_LAND = 6, FLIGHT_MODE_POSHOLD = 7, FLIGHT_MODE_AUTOTUNE = 8, FLIGHT_MODE_ACRO = 9, FLIGHT_MODE_FBWA = 10, FLIGHT_MODE_FBWB = 11, FLIGHT_MODE_CRUISE = 12, FLIGHT_MODE_TAKEOFF = 13, FLIGHT_MODE_RATE = 15, FLIGHT_MODE_HORZ = 16, FLIGHT_MODE_CIRCLE = 17, FLIGHT_MODE_AUTO = 18, FLIGHT_MODE_QSTAB = 20, FLIGHT_MODE_QHOVER = 21, FLIGHT_MODE_QLOITER = 22, FLIGHT_MODE_QLAND = 23, FLIGHT_MODE_QRTL = 24;
    public List<OSDElement> listOSDItems;
    private String currentFCStatus = "";
    private boolean isFlying = false;
    private CountDownTimer mCountDownTimer;
    private boolean osdLocked = true;

    public OSDManager(Context context, ActivityVideoBinding binding) {
        this.binding = binding;
        this.context = context;
    }

    public void lockOSD(Boolean isLocked) {
        for (int i = 0; i < listOSDItems.size(); i++) {
            listOSDItems.get(i).layout.setMovable(!isLocked);
        }
        osdLocked = isLocked;
    }

    public Boolean isOSDLocked() {
        return osdLocked;
    }

    public void onOSDItemCheckChanged(OSDElement element, boolean isChecked) {
        // Show or hide the ImageView corresponding to the checkbox position
        element.layout.setVisibility(isChecked ? View.VISIBLE : View.GONE);
        SharedPreferences prefs = context.getSharedPreferences("osd_config", MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putBoolean(element.prefName() + "_enabled", isChecked);
        editor.apply();
    }

    public void setUp() {
        mCountDownTimer = new CountDownTimer(60 * 60 * 1000, 1000) {
            @Override
            public void onTick(long millisUntilFinished) {
                millisUntilFinished = 60 * 60 * 1000 - millisUntilFinished;
                long minutes = millisUntilFinished / 60000;
                long seconds = (millisUntilFinished % 60000) / 1000;
                binding.tvTimer.setText(String.format("%02d:%02d", minutes, seconds));
            }

            @Override
            public void onFinish() {
            }
        };

        listOSDItems = new ArrayList<OSDElement>();
        listOSDItems.add(new OSDElement("Air Speed", binding.itemAirSpeed));
        listOSDItems.add(new OSDElement("Altitude", binding.itemAlt));
        listOSDItems.add(new OSDElement("Battery Air", binding.itemBat));
        listOSDItems.add(new OSDElement("Battery Cell Air", binding.itemBatCell));
        listOSDItems.add(new OSDElement("Battery GS", binding.itemGSBattery));
        listOSDItems.add(new OSDElement("Current", binding.itemCurrent));
        listOSDItems.add(new OSDElement("Flight Mode", binding.itemFlightMode));
        listOSDItems.add(new OSDElement("Ground Speed", binding.itemGndSpeed));
        listOSDItems.add(new OSDElement("Home Direction", binding.itemHomeNav));
        listOSDItems.add(new OSDElement("Home Distance", binding.itemDis));
        listOSDItems.add(new OSDElement("Latitude", binding.itemLat));
        listOSDItems.add(new OSDElement("Longitude", binding.itemLon));
        listOSDItems.add(new OSDElement("Pitch", binding.itemPitch));
        listOSDItems.add(new OSDElement("RC Link", binding.itemRCLink));
        listOSDItems.add(new OSDElement("Recording Indicator", binding.itemRecIndicator));
        listOSDItems.add(new OSDElement("Roll", binding.itemRoll));
        listOSDItems.add(new OSDElement("Satellites", binding.itemSats));
        listOSDItems.add(new OSDElement("Status", binding.itemStatus));
        listOSDItems.add(new OSDElement("Throttle", binding.itemThrottle));
        listOSDItems.add(new OSDElement("Timer", binding.itemTimer));
        listOSDItems.add(new OSDElement("Total Distance", binding.itemTotDis));
        listOSDItems.add(new OSDElement("Video Decoding", binding.itemVideoStats));
        listOSDItems.add(new OSDElement("Video Link Txt", binding.itemLinkStatus));
        listOSDItems.add(new OSDElement("Video Link Graph", binding.itemLinkStatusChart));

        restoreOSDConfig();
    }

    public boolean isElementEnabled(OSDElement elem) {
        SharedPreferences prefs = context.getSharedPreferences("osd_config", MODE_PRIVATE);
        return prefs.getBoolean(elem.prefName() + "_enabled", false);
    }

    public void restoreOSDConfig() {
        SharedPreferences prefs = context.getSharedPreferences("osd_config", MODE_PRIVATE);
        for (OSDElement element : listOSDItems) {
            boolean enabled = prefs.getBoolean(element.prefName() + "_enabled", false);
            onOSDItemCheckChanged(element, enabled);
            element.layout.restorePosition(element.prefName());
            element.layout.setMovable(!isOSDLocked());
        }
    }

    private float OSDToCourse(double lat1, double long1, double lat2, double long2) {
        double dlon = (long2 - long1) * 0.017453292519;
        lat1 = (lat1) * 0.017453292519;
        lat2 = (lat2) * 0.017453292519;
        double a1 = sin(dlon) * cos(lat2);
        double a2 = sin(lat1) * cos(lat2) * cos(dlon);
        a2 = cos(lat1) * sin(lat2) - a2;
        a2 = atan2(a1, a2);
        if (a2 < 0.0) a2 += 2.0 * 3.141592653589793;
        return (float) (a2 * 180.0 / 3.141592653589793);
    }

    public void render(MavlinkData data) {
        float voltage = (float) (data.telemetryBattery / 1000.0);
        binding.tvBat.setText(formatFloat(voltage, "V", ""));
        int cellCount = (int) (floor(voltage / 4.3) + 1);
        float cellVolt = voltage / cellCount;
        binding.tvBatCell.setText(formatFloat(cellVolt, "V", ""));

        binding.tvCurrent.setText(formatDouble(data.telemetryCurrent / 1000.0, "A", ""));

        binding.tvAlt.setText(formatDouble(data.telemetryAltitude / 100 - 1000, "m", ""));

        binding.tvThrottle.setText(String.format("%.0f", data.telemetryThrottle) + " %\t");

        binding.imgThrottle.setImageResource(data.telemetryArm == 1 ? R.drawable.disarmed : R.drawable.armed);

        if (data.gps_fix_type == 0) {
            binding.tvDis.setText("0 m");
            binding.tvGndSpeed.setText("0 km/h");
            binding.tvAirSpeed.setText("0 km/h");
            binding.tvSats.setText("No GPS");
            binding.tvLat.setText("---");
            binding.tvLon.setText("---");
            //Todo: Home navigation set to default?
        } else {
            if (data.telemetryDistance / 100 > 1000)
                binding.tvDis.setText(formatFloat((float) (data.telemetryDistance / 100000), " km", ""));
            else
                binding.tvDis.setText(formatDouble(data.telemetryDistance / 100, " m", ""));

            binding.tvGndSpeed.setText(formatFloat((float) ((data.telemetryGspeed / 100.0f - 1000.0) * 3.6f), "Km/h", ""));
            binding.tvAirSpeed.setText(formatFloat((float) (data.telemetryVspeed / 100.0f - 1000.0), "m/s", ""));
            binding.tvSats.setText(formatFloat(data.telemetrySats, "", ""));
            binding.tvLat.setText(String.format("%.7f", (float) (data.telemetryLat / 10000000.0f)));
            binding.tvLon.setText(String.format("%.7f", (float) (data.telemetryLon / 10000000.0f)));

            if (data.telemetryArm == 1) {
                float heading_home = OSDToCourse(data.telemetryLat, data.telemetryLon, data.telemetryLatBase, data.telemetryLonBase);
                heading_home = heading_home - 180.0F;

                float rel_heading = heading_home - data.heading;

                rel_heading += 180F;
                if (rel_heading < 0) rel_heading = rel_heading + 360.0F;
                if (rel_heading >= 360) rel_heading = rel_heading - 360.0F;
                binding.tvHeadingHome.setText(formatFloat(heading_home, "", "Heading:"));
                binding.tvRealHeading.setText(formatFloat(rel_heading, "", "Real:"));
                binding.imgHomeNav.setRotation(rel_heading);
            }
        }

        binding.tvRCLink.setText(String.format("%.0f", (float) data.rssi));

        binding.tvRoll.setText(formatFloat(data.telemetryRoll, " degree", ""));
        binding.tvPitch.setText(formatFloat(data.telemetryPitch, " degree", ""));

        String flightMode = "";
        switch (data.flight_mode) {
            case FLIGHT_MODE_MANUAL:
                flightMode = "Manual";
                break;
            case FLIGHT_MODE_STAB:
                flightMode = "Stab";
                break;
            case FLIGHT_MODE_LOITER:
                flightMode = "Loiter";
                break;
            case FLIGHT_MODE_RTL:
                flightMode = "RTL";
                break;
            case FLIGHT_MODE_AUTOTUNE:
                flightMode = "Auto tune";
                break;
            case FLIGHT_MODE_ACRO:
                flightMode = "Acro";
                break;
            case FLIGHT_MODE_FBWA:
                flightMode = "Fbwa";
                break;
            case FLIGHT_MODE_FBWB:
                flightMode = "Fbwb";
                break;
            case FLIGHT_MODE_CRUISE:
                flightMode = "Cruise";
                break;
            case FLIGHT_MODE_TAKEOFF:
                flightMode = "Takeoff";
                break;
            case FLIGHT_MODE_CIRCLE:
                flightMode = "Circle";
                break;
            default:
                flightMode = "Unknown";
                break;
        }
        binding.tvFlightMode.setText(flightMode);

        if (!Objects.equals(currentFCStatus, data.status_text)) {
            currentFCStatus = data.status_text;
            binding.tvStatus.setVisibility(View.VISIBLE);
            binding.tvStatus.setText(data.status_text);
            // Create a Runnable to hide the TextView after 5 second
            Runnable hideTextViewRunnable = new Runnable() {
                @Override
                public void run() {
                    // Hide the TextView
                    binding.tvStatus.setVisibility(View.GONE);
                }
            };

            // Schedule the Runnable to be executed after 1 second (5000 milliseconds)
            handler.postDelayed(hideTextViewRunnable, 5000);
        }

        if (!isFlying && data.telemetryArm == 1) {
            isFlying = true;
            mCountDownTimer.start();
        } else if (data.telemetryArm == 0) {
            isFlying = false;
            mCountDownTimer.cancel();
        }
    }

    private String formatDouble(double v, String unit, String prefix) {
        if (v == 0) {
            return "";
        }
        return String.format("%s%.2f%s", prefix, v, unit);
    }

    private String formatFloat(float v, String unit, String prefix) {
        if (v == 0) {
            return "";
        }
        return String.format("%s%.2f%s", prefix, v, unit);
    }
}
