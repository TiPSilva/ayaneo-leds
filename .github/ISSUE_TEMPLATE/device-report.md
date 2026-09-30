---
name: Device report
about: Tested ayaneo-leds on your Ayaneo? Tell us how it went.
title: "Device report: <model>"
labels: device-report
---

**Model:** <!-- e.g. AYANEO GEEK 1S -->

**collect-report.sh output:**

<!-- paste here -->

**Results** (✅ / ❌ / notes):

- [ ] Loading did not change the rings
- [ ] Red, green and blue show the right colour
- [ ] All four segments of each ring light up
- [ ] Both rings look equally bright (at 255 and at 1)
- [ ] Brightness 1 is dim but still lit
- [ ] Zones: red, green, blue and white each light one segment of the ring
      (where does each colour show up, on the left and on the right ring?)
- [ ] After plugging and unplugging the charger, the colour comes back
- [ ] After suspend and wake, the colour comes back
- [ ] `ec_control` hands the rings back to the EC
- [ ] Unloading works and does not hang

**Anything odd?**

**Photos** (optional, e.g. of the zone test):

**Credit:** may I thank you in the commit that confirms your model? If so, with which name or
GitHub handle?
