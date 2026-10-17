package com.taleworlds.mbwarband;

public final class MBInputStallPolicyTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        MBInputStallPolicy policy = new MBInputStallPolicy();

        check(!policy.shouldConsume(MBInputStallPolicy.ACTION_DOWN, false, false),
                "input must pass through before native startup");
        check(!policy.shouldConsume(MBInputStallPolicy.ACTION_DOWN, true, true),
                "responsive guest input must pass through");

        check(policy.shouldConsume(MBInputStallPolicy.ACTION_DOWN, true, false),
                "a new gesture must be shielded while the guest is stalled");
        check(policy.shouldConsume(2, true, true),
                "a shielded gesture must remain shielded after heartbeat recovery");
        check(policy.shouldConsume(MBInputStallPolicy.ACTION_UP, true, true),
                "the final event of a shielded gesture must also be consumed");
        check(!policy.shouldConsume(2, true, false),
                "events without a shielded ACTION_DOWN must not be captured");

        check(policy.shouldConsume(MBInputStallPolicy.ACTION_DOWN, true, false),
                "second stalled gesture must be shielded");
        check(policy.shouldConsume(MBInputStallPolicy.ACTION_CANCEL, true, false),
                "cancel must finish on the shield side");
        check(!policy.shouldConsume(2, true, false),
                "cancel must reset the shield state");

        policy.shouldConsume(MBInputStallPolicy.ACTION_DOWN, true, false);
        policy.reset();
        check(!policy.shouldConsume(2, true, false), "explicit reset must release the gate");

        System.out.println("MBInputStallPolicyTest: PASS");
    }
}
