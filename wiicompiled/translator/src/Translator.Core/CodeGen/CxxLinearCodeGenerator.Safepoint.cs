using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.CodeGen;

/// <summary>
/// Executor-safepoint planner (SAFEPOINT-DESIGN S1, Route A). The runtime
/// delivers interrupts only from guest activity, so a guest thread spinning in
/// a pump-free poll on callback-maintained state deadlocks against the
/// emulator: nothing it executes ever reaches a delivery point. The cure is a
/// check emitted by the translator itself, immediately before the backward
/// goto of a cycle the planner can prove call-free:
///
///     if (GuestSafepointRequested()) GuestSafepointServe(ctx, 0x&lt;TARGET&gt;u);
///     goto loc_&lt;TARGET&gt;;
///
/// The serve runs on the spinning thread's own execution, under the runtime's
/// deferred-callback discipline; when the request flag is clear the check is
/// one relaxed atomic load and one predicted branch per qualifying loop
/// iteration, and exactly zero everywhere else. No register write-back or
/// reload brackets the call: the delivery entries seed their private callback
/// context from the ambient ctx's r1/r2/r13/FP-mode fields, which are
/// ABI-invariant inside a function body, and the spinner's remaining registers
/// are host locals an ordinary host call preserves.
///
/// Classification is conservative in the direction the design requires: a
/// missed cycle is a missed safepoint, never a wrong one. An edge is planned
/// only when (a) it is a back-edge (target block at or before the branch block
/// in layout order, target guest address at or below the branch block's), and
/// (b) every instruction on the natural loop the edge closes — the target
/// block plus every block that reaches the branch block without passing
/// through the target — lowers to an expression or a FORCE_INLINE memory
/// helper (see <see cref="IsSafepointTransparentInstruction"/>). Functions whose bodies
/// can be re-entered mid-block through ctx-&gt;lr dispatch get no plan at all:
/// no static cycle statement holds for them (the flag-elision abandon
/// precedent).
/// </summary>
public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// The set of (branch block, target block) IR label pairs whose emitted
    /// goto gets a safepoint check. Computed once against the final IR, like
    /// the flag-elision and burst plans, so every emission of a body (discovery,
    /// public entry, state-free fallback) inserts exactly the same checks.
    /// </summary>
    private sealed class SafepointPlan
    {
        public static readonly SafepointPlan Empty = new(new HashSet<(string Block, string Target)>());

        private readonly IReadOnlySet<(string Block, string Target)> _edges;

        public SafepointPlan(IReadOnlySet<(string Block, string Target)> edges) => _edges = edges;

        public IReadOnlySet<(string Block, string Target)> Edges => _edges;

        public bool Covers(string block, string target) => _edges.Contains((block, target));
    }

    private static SafepointPlan PlanSafepoints(
        uint entryPoint,
        IrFunction function,
        IrCfg cfg,
        IReadOnlySet<uint> nonReturningCallTargets,
        IReadOnlySet<uint> lrContinuationCallTargets)
    {
        SafepointCensus.RecordScanned();

        // A body that dispatches on ctx->lr can re-enter any of its own labels,
        // including positions inside a block. The static CFG does not model
        // those edges, so no cycle statement derived from it holds and the
        // whole plan is abandoned for such functions.
        if (HasOutOfBandLocalEntry(function, nonReturningCallTargets, lrContinuationCallTargets))
        {
            SafepointCensus.RecordAbandoned();
            return SafepointPlan.Empty;
        }

        var blocks = function.Blocks;
        var indexByLabel = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var addressByIndex = new uint?[blocks.Count];
        for (var i = 0; i < blocks.Count; ++i)
        {
            indexByLabel[blocks[i].Label] = i;
            if (TryParseAddress(blocks[i].Label, out var address))
            {
                addressByIndex[i] = address;
            }
        }

        // Successor/predecessor indices over the same edges the emitter
        // materialises, for the cycle-membership test below.
        var successors = new List<int>[blocks.Count];
        var predecessors = new List<int>[blocks.Count];
        for (var i = 0; i < blocks.Count; ++i)
        {
            successors[i] = new List<int>();
            predecessors[i] = new List<int>();
        }

        for (var i = 0; i < blocks.Count; ++i)
        {
            foreach (var targetLabel in EmittedGotoEdges(blocks[i], cfg))
            {
                if (indexByLabel.TryGetValue(targetLabel, out var targetIndex) &&
                    !successors[i].Contains(targetIndex))
                {
                    successors[i].Add(targetIndex);
                    predecessors[targetIndex].Add(i);
                }
            }
        }

        var edges = new HashSet<(string Block, string Target)>();
        var sites = new SortedSet<(uint From, uint To)>();
        for (var from = 0; from < blocks.Count; ++from)
        {
            foreach (var targetLabel in EmittedGotoEdges(blocks[from], cfg))
            {
                if (!indexByLabel.TryGetValue(targetLabel, out var to) || to > from)
                {
                    continue; // not a block of this function, or a forward edge
                }

                if (addressByIndex[from] is not { } fromAddress ||
                    addressByIndex[to] is not { } toAddress ||
                    toAddress > fromAddress)
                {
                    continue; // order unprovable without both guest addresses
                }

                SafepointCensus.RecordBackEdge();
                var cycle = CycleBlocks(successors, predecessors, to, from);
                if (cycle is null)
                {
                    // The target cannot reach the branch block, so the edge
                    // closes no cycle: control leaving through it cannot
                    // return here, and nothing can spin on this edge.
                    SafepointCensus.RecordRejected("no_cycle_path");
                    continue;
                }

                var disqualifier = FirstNonTransparentInstruction(blocks, cycle);
                if (disqualifier is not null)
                {
                    SafepointCensus.RecordRejected(disqualifier.Kind);
                    continue;
                }

                if (edges.Add((blocks[from].Label, targetLabel)))
                {
                    sites.Add((fromAddress, toAddress));
                }
            }
        }

        if (edges.Count == 0)
        {
            return SafepointPlan.Empty;
        }

        SafepointCensus.RecordSites(entryPoint, sites);
        return new SafepointPlan(edges);
    }

    /// <summary>
    /// The edges of one block that the body emitter materialises as a
    /// <c>goto</c>, mirroring the terminator switch in EmitFunctionBody case
    /// for case: jump and branch targets by name, jump-table case targets, no
    /// edge for return/undefined (the emitter returns), and the CFG successor
    /// for everything else (including a trailing indirect jump, whose CFG has
    /// no successors by construction).
    /// </summary>
    private static IEnumerable<string> EmittedGotoEdges(IrBasicBlock block, IrCfg cfg)
    {
        var term = block.Instructions.Count > 0 ? block.Instructions[^1] : null;
        switch (term)
        {
            case IrJump jump:
                yield return jump.TargetLabel;
                break;

            case IrBranch branch:
                yield return branch.TrueLabel;
                yield return branch.FalseLabel;
                break;

            case IrJumpTable table:
                foreach (var tableCase in table.Cases)
                {
                    yield return tableCase.TargetLabel;
                }

                break;

            case IrReturn:
            case IrUndefined:
                break;

            default:
                var successor = cfg.Successors(block.Label).FirstOrDefault();
                if (!string.IsNullOrWhiteSpace(successor))
                {
                    yield return successor!;
                }

                break;
        }
    }

    /// <summary>
    /// The natural loop of the back-edge branch -&gt; target: the target block
    /// plus every block that can reach the branch block without passing
    /// through the target. Null when the target cannot reach the branch
    /// block at all, i.e. the edge closes no cycle. Two simpler scans were
    /// tried and rejected on census evidence: a layout-range scan (the
    /// design sketch's wording) both over-rejects — the emitter interleaves
    /// side blocks, such as an exit block holding the function's return,
    /// between a loop head and its compare block — and under-rejects, since
    /// a call whose block sits outside the range can still lie on the cycle
    /// path, which would break the design's invariant that a wrong safepoint
    /// never happens; and the strongly-connected set (blocks reachable from
    /// the target that can reach the branch) over-rejects nested loops,
    /// letting an outer loop's call poison an inner loop's edge even though
    /// the inner cycle never executes it. The natural loop is exactly the
    /// set of blocks an iteration of this cycle can execute.
    /// </summary>
    private static SortedSet<int>? CycleBlocks(
        IReadOnlyList<List<int>> successors,
        IReadOnlyList<List<int>> predecessors,
        int target,
        int branch)
    {
        var forward = new HashSet<int>();
        var stack = new Stack<int>();
        stack.Push(target);
        while (stack.Count > 0)
        {
            var block = stack.Pop();
            if (!forward.Add(block))
            {
                continue;
            }

            foreach (var successor in successors[block])
            {
                stack.Push(successor);
            }
        }

        if (!forward.Contains(branch))
        {
            return null;
        }

        var cycle = new SortedSet<int> { target };
        if (branch != target)
        {
            stack.Push(branch);
            while (stack.Count > 0)
            {
                var block = stack.Pop();
                if (!cycle.Add(block))
                {
                    continue;
                }

                foreach (var predecessor in predecessors[block])
                {
                    stack.Push(predecessor);
                }
            }
        }

        return cycle;
    }

    private static IrInstruction? FirstNonTransparentInstruction(
        IReadOnlyList<IrBasicBlock> blocks, SortedSet<int> cycle)
    {
        foreach (var i in cycle)
        {
            foreach (var instruction in blocks[i].Instructions)
            {
                if (!IsSafepointTransparentInstruction(instruction))
                {
                    return instruction;
                }
            }
        }

        return null;
    }

    /// <summary>
    /// True when the instruction's lowering in EmitInstruction is an expression
    /// or a FORCE_INLINE memory helper, so a cycle containing only transparent
    /// instructions executes no call: the guest can spin in it forever without
    /// reaching any existing delivery point, which is exactly the safepoint's
    /// class. The whitelist is the proven-inline families (assign/binary,
    /// plain and resolved loads/stores, phi, comment, CR-field writes) plus
    /// the pure control transfers (jump/branch, whose lowering is a condition
    /// expression and a goto). Everything else disqualifies by default:
    /// <see cref="IrCall"/> and <see cref="IrIndirectCall"/> are calls;
    /// <see cref="IrResolveGuestMemoryRange"/> lowers to a real runtime call
    /// (MemoryInline::ResolveRangeHost); <see cref="IrJumpTable"/>'s default
    /// case calls InvokeIndirectJump; <see cref="IrIndirectJump"/>,
    /// <see cref="IrReturn"/> and <see cref="IrUndefined"/> lower to dispatch
    /// calls, returns and traps. <see cref="IrTracePpc"/> is transparent: its
    /// lowering in EmitInstruction is an empty case, so it emits no code, makes
    /// no call and cannot be a delivery point; it is an inert marker the lifter
    /// leaves on most instructions. The first planner run proved this matters:
    /// with traces disqualifying, 3,800 of 4,041 back-edges were rejected by
    /// trace alone and coverage was zero, including all six hand-emitted sites
    /// the design validated, whose cycles bear trace markers in the IR exactly
    /// as they bear none in the emitted code.
    /// </summary>
    private static bool IsSafepointTransparentInstruction(IrInstruction instruction) => instruction switch
    {
        IrAssign or IrBinary or
        IrLoad or IrStore or
        IrResolvedLoad or IrResolvedStore or
        IrResolvedPsqLoad or IrResolvedPsqStore or
        IrResolvedLoadPair or IrResolvedStorePair or
        IrPhi or IrComment or IrSetCrField or IrTracePpc or
        IrJump or IrBranch => true,
        _ => false,
    };

    /// <summary>
    /// Emits the safepoint check immediately before the goto for a planned
    /// edge. The serve target is the goto's own destination guest address.
    /// When <paramref name="nextBlockLabels"/> is supplied, an edge to the
    /// layout-next block emits nothing, matching EmitGotoUnlessFallthrough's
    /// elision: a backward edge never targets the layout-next block, but the
    /// check must not be left standing alone if one ever did. Jump-table
    /// gotos are unconditional, so that site passes null.
    /// </summary>
    private static void EmitSafepointCheckIfPlanned(
        StringBuilder sb,
        string pad,
        SafepointPlan plan,
        string currentLabel,
        string targetLabel,
        IReadOnlyDictionary<string, string?>? nextBlockLabels)
    {
        if (plan.Edges.Count == 0 || !plan.Covers(currentLabel, targetLabel))
        {
            return;
        }

        if (nextBlockLabels is not null &&
            nextBlockLabels.TryGetValue(currentLabel, out var nextLabel) &&
            string.Equals(nextLabel, targetLabel, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        if (!TryParseAddress(targetLabel, out var targetAddress))
        {
            return;
        }

        sb.AppendLine($"{pad}if (GuestSafepointRequested()) GuestSafepointServe(ctx, 0x{targetAddress:X8}u);");
    }

    /// <summary>
    /// Process-wide tally of the planner's decisions, printed by the CLI after
    /// a translation so a regeneration's audit can reconcile three ways:
    /// planner census, emitted-check grep census, and the unit-level
    /// strip-and-diff census must all agree. Translation runs in parallel, so
    /// accumulation is locked; recording the same entry twice (a body planned
    /// more than once) overwrites with the identical deterministic result.
    /// </summary>
    private static class SafepointCensus
    {
        private static readonly object Gate = new();
        private static readonly Dictionary<uint, SortedSet<(uint From, uint To)>> SitesByEntry = new();
        private static readonly Dictionary<string, long> RejectedByKind = new(StringComparer.Ordinal);
        private static long _scanned;
        private static long _abandoned;
        private static long _backEdges;

        public static void RecordScanned()
        {
            lock (Gate) { ++_scanned; }
        }

        public static void RecordAbandoned()
        {
            lock (Gate) { ++_abandoned; }
        }

        public static void RecordBackEdge()
        {
            lock (Gate) { ++_backEdges; }
        }

        public static void RecordRejected(string kind)
        {
            lock (Gate)
            {
                RejectedByKind[kind] = RejectedByKind.TryGetValue(kind, out var count) ? count + 1 : 1;
            }
        }

        public static void RecordSites(uint entryPoint, SortedSet<(uint From, uint To)> sites)
        {
            lock (Gate) { SitesByEntry[entryPoint] = sites; }
        }

        public static void Print()
        {
            lock (Gate)
            {
                var siteCount = SitesByEntry.Values.Sum(static sites => sites.Count);
                var rejectedTotal = RejectedByKind.Values.Sum();
                var rejected = RejectedByKind.Count == 0
                    ? "none"
                    : string.Join(", ", RejectedByKind
                        .OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                        .Select(static pair => $"{pair.Key}={pair.Value}"));
                Console.WriteLine(
                    $"[translator] safepoint census: scanned {_scanned} function(s); " +
                    $"abandoned {_abandoned} (out-of-band local entry); " +
                    $"back-edges {_backEdges}; rejected {rejectedTotal} ({rejected}); " +
                    $"planned {siteCount} check(s) in {SitesByEntry.Count} function(s)");
                foreach (var (entryPoint, sites) in SitesByEntry.OrderBy(static pair => pair.Key))
                {
                    var edges = string.Join(" ", sites.Select(
                        static site => $"0x{site.From:X8}->0x{site.To:X8}"));
                    Console.WriteLine($"[translator] safepoint sites func_{entryPoint:X8}: {edges}");
                }
            }
        }
    }

    /// <summary>Prints the safepoint planner census; called by the CLI once a translation completes.</summary>
    public static void PrintSafepointCensus() => SafepointCensus.Print();
}
