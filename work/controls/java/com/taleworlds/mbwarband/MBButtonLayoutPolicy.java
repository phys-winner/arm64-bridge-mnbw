package com.taleworlds.mbwarband;

/** Android-free geometry policy for the mission touch buttons. */
public final class MBButtonLayoutPolicy {
    public static final float MIN_SCALE = 0.6f;
    public static final float MAX_SCALE = 1.4f;
    private static final float PHONE_REFERENCE_SHORTEST_DP = 480.0f;
    private static final float MAX_DENSITY_AWARE_SCALE = 1.65f;
    private static final float LARGE_SCREEN_SCALE_EXPONENT = 0.8f;
    private static final int SCALE_SEARCH_STEPS = 24;
    private static final int PACKING_ITERATIONS = 768;
    private static final float PACKING_EPSILON = 0.02f;

    private MBButtonLayoutPolicy() {
    }

    public static boolean finite(float value) {
        return !Float.isNaN(value) && !Float.isInfinite(value);
    }

    public static float requestedScale(float value) {
        if (!finite(value)) return 1.0f;
        return Math.max(MIN_SCALE, Math.min(MAX_SCALE, value));
    }

    /**
     * Enlarges controls on large screens while retaining Android's physical-density scaling.
     * Phones at or below 480dp on the short edge keep the original size. Larger displays grow
     * sub-linearly, capped so a user-selected button scale still has useful headroom. The
     * deliberately stronger exponent keeps controls physically comfortable on 8--12 inch
     * tablets instead of merely compensating for their lower logical density.
     */
    public static float densityAwareScale(float widthPixels, float heightPixels,
                                          float density) {
        if (!finite(widthPixels) || !finite(heightPixels) || !finite(density)
                || widthPixels <= 0.0f || heightPixels <= 0.0f || density <= 0.0f) {
            return 1.0f;
        }
        float shortestDp = Math.min(widthPixels, heightPixels) / density;
        if (!finite(shortestDp) || shortestDp <= PHONE_REFERENCE_SHORTEST_DP) return 1.0f;
        return Math.min(MAX_DENSITY_AWARE_SCALE,
                (float)Math.pow(shortestDp / PHONE_REFERENCE_SHORTEST_DP,
                        LARGE_SCREEN_SCALE_EXPONENT));
    }

    /**
     * Pulls a coordinate toward its nearest edge while keeping the centre and both edges fixed.
     * Unlike a simple edge-distance division this mapping is continuous and reversible, so a
     * button dragged anywhere on the display can still be stored without a dead zone.
     */
    public static float compactTowardNearestEdge(float value, float span, float factor) {
        if (!finite(span) || span <= 0.0f) return 0.0f;
        if (!finite(value)) value = span * 0.5f;
        float safeFactor = finite(factor) ? Math.max(1.0f, factor) : 1.0f;
        float normalized = Math.max(0.0f, Math.min(1.0f, value / span));
        float signedDistance = (normalized - 0.5f) * 2.0f;
        float magnitude = (float)Math.pow(Math.abs(signedDistance), 1.0f / safeFactor);
        return (0.5f + Math.copySign(magnitude * 0.5f, signedDistance)) * span;
    }

    /** Inverse of {@link #compactTowardNearestEdge(float, float, float)}. */
    public static float expandFromNearestEdge(float value, float span, float factor) {
        if (!finite(span) || span <= 0.0f) return 0.0f;
        if (!finite(value)) value = span * 0.5f;
        float safeFactor = finite(factor) ? Math.max(1.0f, factor) : 1.0f;
        float normalized = Math.max(0.0f, Math.min(1.0f, value / span));
        float signedDistance = (normalized - 0.5f) * 2.0f;
        float magnitude = (float)Math.pow(Math.abs(signedDistance), safeFactor);
        return (0.5f + Math.copySign(magnitude * 0.5f, signedDistance)) * span;
    }

    public static float clampCenter(float value, float minimum, float maximum) {
        if (!finite(value)) value = (minimum + maximum) * 0.5f;
        if (!finite(minimum) || !finite(maximum)) return 0.0f;
        if (minimum > maximum) return (minimum + maximum) * 0.5f;
        return Math.max(minimum, Math.min(maximum, value));
    }

    private static boolean validArrays(float[] rawX, float[] rawY, float[] baseRadius) {
        return rawX != null && rawY != null && baseRadius != null
                && rawX.length == rawY.length && rawX.length == baseRadius.length
                && rawX.length > 0;
    }

    private static boolean validOutputArrays(float[] rawX, float[] centerX,
                                             float[] centerY, float[] radius) {
        return rawX != null && centerX != null && centerY != null && radius != null
                && centerX.length == rawX.length && centerY.length == rawX.length
                && radius.length == rawX.length;
    }

    private static float directionX(int first, int second) {
        final float diagonal = 0.70710677f;
        switch (((first + 1) * 11 + (second + 1) * 7) & 7) {
            case 0: return 1.0f;
            case 1: return diagonal;
            case 2: return 0.0f;
            case 3: return -diagonal;
            case 4: return -1.0f;
            case 5: return -diagonal;
            case 6: return 0.0f;
            default: return diagonal;
        }
    }

    private static float directionY(int first, int second) {
        final float diagonal = 0.70710677f;
        switch (((first + 1) * 11 + (second + 1) * 7) & 7) {
            case 0: return 0.0f;
            case 1: return diagonal;
            case 2: return 1.0f;
            case 3: return diagonal;
            case 4: return 0.0f;
            case 5: return -diagonal;
            case 6: return -1.0f;
            default: return -diagonal;
        }
    }

    private static boolean layoutIsValid(float width, float height,
                                         float safeLeft, float safeTop,
                                         float safeRight, float safeBottom,
                                         float gap, float[] centerX,
                                         float[] centerY, float[] radius) {
        final float rightEdge = width - safeRight;
        final float bottomEdge = height - safeBottom;
        for (int i = 0; i < centerX.length; ++i) {
            if (!finite(centerX[i]) || !finite(centerY[i]) || !finite(radius[i])
                    || radius[i] <= 0.0f
                    || centerX[i] - radius[i] < safeLeft - PACKING_EPSILON
                    || centerX[i] + radius[i] > rightEdge + PACKING_EPSILON
                    || centerY[i] - radius[i] < safeTop - PACKING_EPSILON
                    || centerY[i] + radius[i] > bottomEdge + PACKING_EPSILON) {
                return false;
            }
            for (int j = 0; j < i; ++j) {
                double dx = (double) centerX[i] - centerX[j];
                double dy = (double) centerY[i] - centerY[j];
                double required = (double) radius[i] + radius[j] + gap;
                if (dx * dx + dy * dy + PACKING_EPSILON < required * required) {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * Attempts to pack the requested circles while keeping their initial positions as
     * anchors. Pair resolution is deliberately deterministic: painter order is alternated
     * on successive passes and coincident centres use a stable index-derived direction.
     */
    private static boolean packAtScale(float width, float height,
                                       float safeLeft, float safeTop,
                                       float safeRight, float safeBottom,
                                       float scale, float gap, float minimumRadius,
                                       float[] rawX, float[] rawY,
                                       float[] baseRadius,
                                       float[] centerX, float[] centerY,
                                       float[] radius) {
        if (!validArrays(rawX, rawY, baseRadius) || !finite(width) || !finite(height)
                || !validOutputArrays(rawX, centerX, centerY, radius)
                || width <= 0.0f || height <= 0.0f || !finite(scale) || scale <= 0.0f) {
            return false;
        }
        final float rightEdge = width - safeRight;
        final float bottomEdge = height - safeBottom;
        boolean individuallyReachable = true;
        for (int i = 0; i < rawX.length; ++i) {
            radius[i] = Math.max(baseRadius[i] * scale, minimumRadius);
            if (!finite(radius[i]) || radius[i] <= 0.0f
                    || safeLeft + radius[i] > rightEdge - radius[i]
                    || safeTop + radius[i] > bottomEdge - radius[i]) {
                individuallyReachable = false;
            }
            centerX[i] = clampCenter(rawX[i], safeLeft + radius[i], rightEdge - radius[i]);
            centerY[i] = clampCenter(rawY[i], safeTop + radius[i], bottomEdge - radius[i]);
        }
        if (!individuallyReachable) return false;

        for (int iteration = 0; iteration < PACKING_ITERATIONS; ++iteration) {
            boolean hadOverlap = false;
            final boolean reverse = (iteration & 1) != 0;
            for (int outer = 0; outer < rawX.length; ++outer) {
                int first = reverse ? rawX.length - 1 - outer : outer;
                for (int inner = outer + 1; inner < rawX.length; ++inner) {
                    int second = reverse ? rawX.length - 1 - inner : inner;
                    double dx = (double) centerX[first] - centerX[second];
                    double dy = (double) centerY[first] - centerY[second];
                    double distanceSquared = dx * dx + dy * dy;
                    float required = radius[first] + radius[second] + gap;
                    if (distanceSquared + PACKING_EPSILON >= (double) required * required) {
                        continue;
                    }
                    hadOverlap = true;

                    float unitX;
                    float unitY;
                    double distance = Math.sqrt(distanceSquared);
                    if (distance > 0.0001) {
                        unitX = (float) (dx / distance);
                        unitY = (float) (dy / distance);
                    } else {
                        int low = Math.min(first, second);
                        int high = Math.max(first, second);
                        unitX = directionX(low, high);
                        unitY = directionY(low, high);
                        if (first > second) {
                            unitX = -unitX;
                            unitY = -unitY;
                        }
                        distance = 0.0;
                    }

                    // The small over-correction prevents floating-point stalls at contact.
                    float displacement = ((float) (required - distance) + PACKING_EPSILON) * 0.5f;
                    centerX[first] = clampCenter(centerX[first] + unitX * displacement,
                            safeLeft + radius[first], rightEdge - radius[first]);
                    centerY[first] = clampCenter(centerY[first] + unitY * displacement,
                            safeTop + radius[first], bottomEdge - radius[first]);
                    centerX[second] = clampCenter(centerX[second] - unitX * displacement,
                            safeLeft + radius[second], rightEdge - radius[second]);
                    centerY[second] = clampCenter(centerY[second] - unitY * displacement,
                            safeTop + radius[second], bottomEdge - radius[second]);
                }
            }
            if (!hadOverlap || layoutIsValid(width, height, safeLeft, safeTop,
                    safeRight, safeBottom, gap, centerX, centerY, radius)) {
                return true;
            }
        }
        return layoutIsValid(width, height, safeLeft, safeTop, safeRight, safeBottom,
                gap, centerX, centerY, radius);
    }

    /**
     * Resolves safe, non-overlapping centres and radii, returning the scale actually used.
     * The requested scale is tried first. Binary scale reduction is used only when packing
     * that scale is geometrically unsuccessful. If even 0.6x cannot fit, a clamped 0.6x
     * fallback is emitted; deterministic nearest-centre hit arbitration then resolves the
     * unavoidable overlap.
     */
    public static float resolveLayout(float width, float height,
                                      int safeLeft, int safeTop,
                                      int safeRight, int safeBottom,
                                      float requested, float gap,
                                      float minimumConstraintRadius,
                                      float[] rawX, float[] rawY,
                                      float[] baseRadius,
                                      float[] centerX, float[] centerY,
                                      float[] radius) {
        float target = requestedScale(requested);
        float left = Math.max(0, safeLeft);
        float top = Math.max(0, safeTop);
        float right = Math.max(0, safeRight);
        float bottom = Math.max(0, safeBottom);
        float safeGap = finite(gap) ? Math.max(0.0f, gap) : 0.0f;
        float safeMinimumRadius = finite(minimumConstraintRadius)
                ? Math.max(0.0f, minimumConstraintRadius) : 0.0f;
        if (!validArrays(rawX, rawY, baseRadius)
                || !validOutputArrays(rawX, centerX, centerY, radius)) {
            return MIN_SCALE;
        }
        if (packAtScale(width, height, left, top, right, bottom, target, safeGap,
                safeMinimumRadius,
                rawX, rawY, baseRadius, centerX, centerY, radius)) {
            writeVisualRadii(target, baseRadius, radius);
            return target;
        }
        if (!packAtScale(width, height, left, top, right, bottom, MIN_SCALE, safeGap,
                safeMinimumRadius,
                rawX, rawY, baseRadius, centerX, centerY, radius)) {
            // packAtScale still emits the individually clamped minimum-scale layout.
            writeVisualRadii(MIN_SCALE, baseRadius, radius);
            return MIN_SCALE;
        }
        float low = MIN_SCALE;
        float high = target;
        for (int step = 0; step < SCALE_SEARCH_STEPS; ++step) {
            float middle = (low + high) * 0.5f;
            if (packAtScale(width, height, left, top, right, bottom, middle, safeGap,
                    safeMinimumRadius,
                    rawX, rawY, baseRadius, centerX, centerY, radius)) {
                low = middle;
            } else {
                high = middle;
            }
        }
        packAtScale(width, height, left, top, right, bottom, low, safeGap,
                safeMinimumRadius,
                rawX, rawY, baseRadius, centerX, centerY, radius);
        writeVisualRadii(low, baseRadius, radius);
        return low;
    }

    public static float resolveLayout(float width, float height,
                                      int safeLeft, int safeTop,
                                      int safeRight, int safeBottom,
                                      float requested, float gap,
                                      float[] rawX, float[] rawY,
                                      float[] baseRadius,
                                      float[] centerX, float[] centerY,
                                      float[] radius) {
        return resolveLayout(width, height, safeLeft, safeTop, safeRight, safeBottom,
                requested, gap, 0.0f, rawX, rawY, baseRadius,
                centerX, centerY, radius);
    }

    private static void writeVisualRadii(float scale, float[] baseRadius, float[] radius) {
        for (int index = 0; index < radius.length; ++index) {
            radius[index] = baseRadius[index] * scale;
        }
    }

    /** Compatibility wrapper for callers interested only in the packed scale. */
    public static float effectiveScale(float width, float height,
                                       int safeLeft, int safeTop,
                                       int safeRight, int safeBottom,
                                       float requested, float gap,
                                       float[] rawX, float[] rawY,
                                       float[] baseRadius) {
        if (!validArrays(rawX, rawY, baseRadius)) return MIN_SCALE;
        int count = rawX.length;
        return resolveLayout(width, height, safeLeft, safeTop, safeRight, safeBottom,
                requested, gap, rawX, rawY, baseRadius,
                new float[count], new float[count], new float[count]);
    }

    /** Returns a painter-order index, with later-painted circles winning an exact tie. */
    public static int hitIndex(float x, float y, float[] centerX, float[] centerY,
                               float[] radius) {
        return hitIndex(x, y, centerX, centerY, radius, null);
    }

    /**
     * Returns a painter-order index while ignoring disabled controls. Filtering during
     * arbitration prevents a hidden overlapping circle from stealing a visible hit.
     */
    public static int hitIndex(float x, float y, float[] centerX, float[] centerY,
                               float[] radius, boolean[] enabled) {
        if (!finite(x) || !finite(y) || !validArrays(centerX, centerY, radius)) return -1;
        if (enabled != null && enabled.length != centerX.length) return -1;
        int best = -1;
        float bestNormalizedDistance = Float.POSITIVE_INFINITY;
        for (int index = 0; index < centerX.length; ++index) {
            if (enabled != null && !enabled[index]) continue;
            float r = radius[index];
            if (!finite(centerX[index]) || !finite(centerY[index])
                    || !finite(r) || r <= 0.0f) {
                continue;
            }
            float dx = x - centerX[index];
            float dy = y - centerY[index];
            float normalizedDistance = (dx * dx + dy * dy) / (r * r);
            if (normalizedDistance <= 1.0f
                    && normalizedDistance <= bestNormalizedDistance) {
                best = index;
                bestNormalizedDistance = normalizedDistance;
            }
        }
        return best;
    }
}
