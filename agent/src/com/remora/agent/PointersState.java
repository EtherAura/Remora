package com.remora.agent;

import android.view.MotionEvent;

import java.util.ArrayList;
import java.util.List;

/**
 * Tracks the live pointers of one control session and materializes them into the parallel
 * PointerProperties/PointerCoords arrays MotionEvent.obtain() wants. A pointer's key is the
 * client's tool and pointer id together (Controller.pointerKey), so a finger and the mouse never
 * share a slot; each key maps to the lowest free local id, and UP pointers are reaped after each
 * update so a 10-finger session never leaks slots.
 */
public final class PointersState {
    public static final int MAX_POINTERS = 10;

    static final class Pointer {
        final long id;      // pointer key: tool and wire id
        final int localId;  // dense id for PointerProperties
        float x, y, pressure;
        boolean up;

        Pointer(long id, int localId) {
            this.id = id;
            this.localId = localId;
        }
    }

    private final List<Pointer> pointers = new ArrayList<>();

    private boolean isLocalIdUsed(int localId) {
        for (Pointer p : pointers) {
            if (p.localId == localId) return true;
        }
        return false;
    }

    /** Index of the pointer with this key, creating it if room remains; -1 when full. */
    public int getPointerIndex(long id) {
        for (int i = 0; i < pointers.size(); ++i) {
            if (pointers.get(i).id == id) return i;
        }
        if (pointers.size() >= MAX_POINTERS) return -1;
        int localId = 0;
        while (isLocalIdUsed(localId)) ++localId;
        pointers.add(new Pointer(id, localId));
        return pointers.size() - 1;
    }

    public Pointer get(int index) {
        return pointers.get(index);
    }

    /** Fills props/coords for the current pointers, reaps UP ones, returns the count filled. */
    public int update(MotionEvent.PointerProperties[] props, MotionEvent.PointerCoords[] coords) {
        int count = pointers.size();
        for (int i = 0; i < count; ++i) {
            Pointer p = pointers.get(i);
            props[i].id = p.localId;
            coords[i].x = p.x;
            coords[i].y = p.y;
            coords[i].pressure = p.pressure;
        }
        for (int i = pointers.size() - 1; i >= 0; --i) {
            if (pointers.get(i).up) pointers.remove(i);
        }
        return count;
    }
}
