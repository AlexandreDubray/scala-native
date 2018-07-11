package scala.scalanative
package optimizer
package pass

import scala.collection.mutable
import scalanative.nir._
import scalanative.optimizer.analysis.ClassHierarchy.Top

class InsertWriteBarrier extends Pass {
  import InsertWriteBarrier._

  override def onInsts(insts: Seq[Inst]): Seq[Inst] = {
    val buf    = new nir.Buffer
    val fields = mutable.Map.empty[Local, Val]

    insts.foreach {
      case Inst.Let(n, Op.Field(obj, _)) =>
        fields(n) = obj
      case _ =>
        ()
    }

    insts.foreach {
      case inst @ Inst.Let(_, Op.Store(_: Type.RefKind, Val.Local(n, _), _, _))
        if fields.contains(n) =>
        buf += inst
        buf.call(barrierSig, barrier, Seq(fields(n)), Next.None)
      case inst =>
        buf += inst
    }

    buf.toSeq
  }
}

object InsertWriteBarrier extends PassCompanion {
  val barrierName = Global.Top("scalanative_write_barrier")
  val barrierSig  = Type.Function(Seq(Type.Ptr), Type.Unit)
  val barrier     = Val.Global(barrierName, barrierSig)

  override val injects =
    Seq(Defn.Declare(Attrs.None, barrierName, barrierSig))

  override def apply(config: build.Config, top: Top) = {
    assert(config.gc.needsWriteBarrier)
    new InsertWriteBarrier
  }
}
