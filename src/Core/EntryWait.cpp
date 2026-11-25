#include "Core/EntryWait.h"

namespace Core
{
	StanceCallResult DecideStanceCall(const StanceCallInput& a_in)
	{
		if (a_in.callsMade <= 0) {
			if (a_in.sinceSwitch < a_in.switchWait) {
				return { false, "switch wait" };
			}
			if (!a_in.waitForByte) {
				return { true, "waiting for the byte is off" };
			}
			if (a_in.pendingByte < 0) {
				return { true, "the byte cannot be read" };
			}
			if (a_in.pendingByte == 0) {
				if (a_in.waitForEquipByte && a_in.equipByte > 0) {
					if (a_in.sinceSwitch >= a_in.maxWait) {
						return { true, "the limit passed with the equip byte still owed" };
					}
					return { false, "the equip byte is owed" };
				}
				return { true, a_in.waitForEquipByte ? "both bytes are clear" : "the byte is clear" };
			}
			if (a_in.sinceSwitch >= a_in.maxWait) {
				return { true, "the limit passed with the byte still pending" };
			}
			return { false, "the byte is pending" };
		}
		if (a_in.noRepeat) {
			return { false, "no repeat (iPoseNoRepeat)" };
		}
		if (a_in.callsMade >= a_in.maxCalls || a_in.sinceLastCall <= a_in.repeatAfter) {
			return { false, "no repeat yet" };
		}
		if (!a_in.waitForByte || a_in.pendingByte <= 0) {
			return { true, "repeat" };
		}
		if (a_in.sinceLastCall >= a_in.maxWait) {
			return { true, "repeat, the limit passed with the byte still pending" };
		}
		return { false, "the byte is pending" };
	}
}
