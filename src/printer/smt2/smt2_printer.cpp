/*********************                                                        */
/*! \file smt2_printer.cpp
 ** \verbatim
 ** Original author: Morgan Deters
 ** Major contributors: none
 ** Minor contributors (to current version): Dejan Jovanovic, Tim King, Liana Hadarean, Kshitij Bansal, Tianyi Liang, Francois Bobot, Andrew Reynolds
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2014  New York University and The University of Iowa
 ** See the file COPYING in the top-level source directory for licensing
 ** information.\endverbatim
 **
 ** \brief The pretty-printer interface for the SMT2 output language
 **
 ** The pretty-printer interface for the SMT2 output language.
 **/

#include "printer/smt2/smt2_printer.h"

#include <iostream>
#include <vector>
#include <string>
#include <typeinfo>

#include "util/boolean_simplification.h"
#include "printer/dagification_visitor.h"
#include "util/node_visitor.h"
#include "theory/substitutions.h"
#include "util/language.h"
#include "smt/smt_engine.h"
#include "smt/options.h"
#include "expr/node_manager_attributes.h"

#include "theory/theory_model.h"
#include "theory/arrays/theory_arrays_rewriter.h"
#include "theory/quantifiers/term_database.h"

using namespace std;

namespace CVC4 {
namespace printer {
namespace smt2 {

static string smtKindString(Kind k) throw();

static void printBvParameterizedOp(std::ostream& out, TNode n) throw();
static void printFpParameterizedOp(std::ostream& out, TNode n) throw();

static void toStreamRational(std::ostream& out, const Rational& r, bool decimal) throw();
  
void Smt2Printer::toStream(std::ostream& out, TNode n,
                           int toDepth, bool types, size_t dag) const throw() {
  if(dag != 0) {
    DagificationVisitor dv(dag);
    NodeVisitor<DagificationVisitor> visitor;
    visitor.run(dv, n);
    const theory::SubstitutionMap& lets = dv.getLets();
    if(!lets.empty()) {
      theory::SubstitutionMap::const_iterator i = lets.begin();
      theory::SubstitutionMap::const_iterator i_end = lets.end();
      for(; i != i_end; ++ i) {
        out << "(let ((";
        toStream(out, (*i).second, toDepth, types);
        out << ' ';
        toStream(out, (*i).first, toDepth, types);
        out << ")) ";
      }
    }
    Node body = dv.getDagifiedBody();
    toStream(out, body, toDepth, types);
    if(!lets.empty()) {
      theory::SubstitutionMap::const_iterator i = lets.begin();
      theory::SubstitutionMap::const_iterator i_end = lets.end();
      for(; i != i_end; ++ i) {
        out << ")";
      }
    }
  } else {
    toStream(out, n, toDepth, types);
  }
}

static std::string maybeQuoteSymbol(const std::string& s) {
  // this is the set of SMT-LIBv2 permitted characters in "simple" (non-quoted) symbols
  if(s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789~!@$%^&*_-+=<>.?/") != string::npos) {
    // need to quote it
    stringstream ss;
    ss << '|' << s << '|';
    return ss.str();
  }
  return s;
}

static bool stringifyRegexp(Node n, stringstream& ss) {
  if(n.getKind() == kind::STRING_TO_REGEXP) {
    ss << n[0].getConst<String>().toString();
  } else if(n.getKind() == kind::REGEXP_CONCAT) {
    for(unsigned i = 0; i < n.getNumChildren(); ++i) {
      if(!stringifyRegexp(n[i], ss)) {
        return false;
      }
    }
  } else {
    return false;
  }
  return true;
}

void Smt2Printer::toStream(std::ostream& out, TNode n,
                           int toDepth, bool types) const throw() {
  // null
  if(n.getKind() == kind::NULL_EXPR) {
    out << "null";
    return;
  }

  // variable
  if(n.isVar()) {
    string s;
    if(n.getAttribute(expr::VarNameAttr(), s)) {
      out << maybeQuoteSymbol(s);
    } else {
      if(n.getKind() == kind::VARIABLE) {
        out << "var_";
      } else {
        out << n.getKind() << '_';
      }
      out << n.getId();
    }
    if(types) {
      // print the whole type, but not *its* type
      out << ":";
      n.getType().toStream(out, language::output::LANG_SMTLIB_V2_5);
    }

    return;
  }

  // constant
  if(n.getMetaKind() == kind::metakind::CONSTANT) {
    switch(n.getKind()) {
    case kind::TYPE_CONSTANT:
      switch(n.getConst<TypeConstant>()) {
      case BOOLEAN_TYPE: out << "Bool"; break;
      case REAL_TYPE: out << "Real"; break;
      case INTEGER_TYPE: out << "Int"; break;
      case STRING_TYPE: out << "String"; break;
      case ROUNDINGMODE_TYPE: out << "RoundingMode"; break;
      default:
        // fall back on whatever operator<< does on underlying type; we
        // might luck out and be SMT-LIB v2 compliant
        kind::metakind::NodeValueConstPrinter::toStream(out, n);
      }
      break;
    case kind::BITVECTOR_TYPE:
      out << "(_ BitVec " << n.getConst<BitVectorSize>().size << ")";
      break;
    case kind::FLOATINGPOINT_TYPE:
      out << "(_ FloatingPoint "
	  << n.getConst<FloatingPointSize>().exponent() << " "
	  << n.getConst<FloatingPointSize>().significand()
	  << ")";
      break;
    case kind::CONST_BITVECTOR: {
      const BitVector& bv = n.getConst<BitVector>();
      const Integer& x = bv.getValue();
      unsigned n = bv.getSize();
      out << "(_ ";
      out << "bv" << x <<" " << n;
      out << ")";
      // //out << "#b";

      // while(n-- > 0) {
      //   out << (x.testBit(n) ? '1' : '0');
      // }
      break;
    }
    case kind::CONST_FLOATINGPOINT:
      out << n.getConst<FloatingPoint>().getLiteral();
      break;
    case kind::CONST_ROUNDINGMODE:
      switch (n.getConst<RoundingMode>()) {
      case roundNearestTiesToEven : out << "roundNearestTiesToEven"; break;
      case roundNearestTiesToAway : out << "roundNearestTiesToAway"; break;
      case roundTowardPositive : out << "roundTowardPositive"; break;
      case roundTowardNegative : out << "roundTowardNegative"; break;
      case roundTowardZero : out << "roundTowardZero"; break;
      default :
	Unreachable("Invalid value of rounding mode constant (%d)",n.getConst<RoundingMode>());
      }
      break;
    case kind::CONST_BOOLEAN:
      // the default would print "1" or "0" for bool, that's not correct
      // for our purposes
      out << (n.getConst<bool>() ? "true" : "false");
      break;
    case kind::BUILTIN:
      out << smtKindString(n.getConst<Kind>());
      break;
    case kind::CHAIN_OP:
      out << smtKindString(n.getConst<Chain>().getOperator());
      break;
    case kind::CONST_RATIONAL: {
      const Rational& r = n.getConst<Rational>();
      toStreamRational(out, r, false);
      // Rational r = n.getConst<Rational>();
      // if(r < 0) {
      //   if(r.isIntegral()) {
      //     out << "(- " << -r << ')';
      //   } else {
      //     out << "(- (/ " << (-r).getNumerator() << ' ' << (-r).getDenominator() << "))";
      //   }
      // } else {
      //   if(r.isIntegral()) {
      //     out << r;
      //   } else {
      //     out << "(/ " << r.getNumerator() << ' ' << r.getDenominator() << ')';
      //   }
      // }
      break;
    }

    case kind::CONST_STRING: {
      //const std::vector<unsigned int>& s = n.getConst<String>().getVec();
      std::string s = n.getConst<String>().toString();
      out << '"';
      for(size_t i = 0; i < s.size(); ++i) {
        //char c = String::convertUnsignedIntToChar(s[i]);
        char c = s[i];
        if(c == '"') {
          if(d_variant == smt2_0_variant) {
            out << "\\\"";
          } else {
            out << "\"\"";
          }
        } else {
          out << c;
        }
      }
      out << '"';
      break;
    }

    case kind::STORE_ALL: {
      ArrayStoreAll asa = n.getConst<ArrayStoreAll>();
      out << "((as const " << asa.getType() << ") " << asa.getExpr() << ")";
      break;
    }

    case kind::SUBRANGE_TYPE: {
      const SubrangeBounds& bounds = n.getConst<SubrangeBounds>();
      // No way to represent subranges in SMT-LIBv2; this is inspired
      // by yices format (but isn't identical to it).
      out << "(subrange " << bounds.lower << ' ' << bounds.upper << ')';
      break;
    }
    case kind::SUBTYPE_TYPE:
      // No way to represent predicate subtypes in SMT-LIBv2; this is
      // inspired by yices format (but isn't identical to it).
      out << "(subtype " << n.getConst<Predicate>() << ')';
      break;

    case kind::DATATYPE_TYPE:
      out << maybeQuoteSymbol(n.getConst<Datatype>().getName());
      break;

    case kind::UNINTERPRETED_CONSTANT: {
      const UninterpretedConstant& uc = n.getConst<UninterpretedConstant>();
      out << '@' << uc;
      break;
    }

    case kind::EMPTYSET:
      out << "(as emptyset " << n.getConst<EmptySet>().getType() << ")";
      break;

    default:
      // fall back on whatever operator<< does on underlying type; we
      // might luck out and be SMT-LIB v2 compliant
      kind::metakind::NodeValueConstPrinter::toStream(out, n);
    }

    return;
  }

  if(n.getKind() == kind::SORT_TYPE) {
    string name;
    if(n.getAttribute(expr::VarNameAttr(), name)) {
      out << maybeQuoteSymbol(name);
      return;
    }
  }

  bool stillNeedToPrintParams = true;
  bool forceBinary = false; // force N-ary to binary when outputing children
  // operator
  if(n.getNumChildren() != 0 &&
     n.getKind() != kind::INST_PATTERN_LIST &&
     n.getKind() != kind::APPLY_TYPE_ASCRIPTION) {
    out << '(';
  }
  switch(Kind k = n.getKind()) {
    // builtin theory
  case kind::APPLY: break;
  case kind::EQUAL:
  case kind::DISTINCT: out << smtKindString(k) << " "; break;
  case kind::CHAIN: break;
  case kind::TUPLE: break;
  case kind::FUNCTION_TYPE:
    for(size_t i = 0; i < n.getNumChildren() - 1; ++i) {
      if(i > 0) {
        out << ' ';
      }
      out << n[i];
    }
    out << ") " << n[n.getNumChildren() - 1];
    return;
  case kind::SEXPR: break;

    // bool theory
  case kind::NOT:
  case kind::AND:
  case kind::IFF:
  case kind::IMPLIES:
  case kind::OR:
  case kind::XOR:
  case kind::ITE: out << smtKindString(k) << " "; break;

    // uf theory
  case kind::APPLY_UF: break;

    // arith theory
  case kind::PLUS:
  case kind::MULT:
  case kind::MINUS:
  case kind::UMINUS:
  case kind::LT:
  case kind::LEQ:
  case kind::GT:
  case kind::GEQ:
  case kind::DIVISION:
  case kind::DIVISION_TOTAL:
  case kind::INTS_DIVISION:
  case kind::INTS_DIVISION_TOTAL:
  case kind::INTS_MODULUS:
  case kind::INTS_MODULUS_TOTAL:
  case kind::ABS:
  case kind::IS_INTEGER:
  case kind::TO_INTEGER:
  case kind::TO_REAL:
  case kind::POW: out << smtKindString(k) << " "; break;

  case kind::DIVISIBLE:
    out << "(_ divisible " << n.getOperator().getConst<Divisible>().k << ")";
    stillNeedToPrintParams = false;
    break;

    // arrays theory
  case kind::SELECT:
  case kind::STORE:
  case kind::ARRAY_TYPE: out << smtKindString(k) << " "; break;

    // string theory
  case kind::STRING_CONCAT:
    if(d_variant == z3str_variant) {
      out << "Concat ";
      for(unsigned i = 0; i < n.getNumChildren(); ++i) {
        toStream(out, n[i], -1, types);
        if(i + 1 < n.getNumChildren()) {
          out << ' ';
        }
        if(i + 2 < n.getNumChildren()) {
          out << "(Concat ";
        }
      }
      for(unsigned i = 0; i < n.getNumChildren() - 1; ++i) {
        out << ")";
      }
      return;
    }
    out << "str.++ ";
    break;
  case kind::STRING_IN_REGEXP: {
    stringstream ss;
    if(d_variant == z3str_variant && stringifyRegexp(n[1], ss)) {
      out << "= ";
      toStream(out, n[0], -1, types);
      out << " ";
      Node str = NodeManager::currentNM()->mkConst(String(ss.str()));
      toStream(out, str, -1, types);
      out << ")";
      return;
    }
    out << "str.in.re ";
    break;
  }
  case kind::STRING_LENGTH: out << (d_variant == z3str_variant ? "Length " : "str.len "); break;
  case kind::STRING_SUBSTR_TOTAL:
  case kind::STRING_SUBSTR: out << "str.substr "; break;
  case kind::STRING_CHARAT: out << "str.at "; break;
  case kind::STRING_STRCTN: out << "str.contains "; break;
  case kind::STRING_STRIDOF: out << "str.indexof "; break;
  case kind::STRING_STRREPL: out << "str.replace "; break;
  case kind::STRING_PREFIX: out << "str.prefixof "; break;
  case kind::STRING_SUFFIX: out << "str.suffixof "; break;
  case kind::STRING_ITOS: out << "int.to.str "; break;
  case kind::STRING_STOI: out << "str.to.int "; break;
  case kind::STRING_U16TOS: out << "u16.to.str "; break;
  case kind::STRING_STOU16: out << "str.to.u16 "; break;
  case kind::STRING_U32TOS: out << "u32.to.str "; break;
  case kind::STRING_STOU32: out << "str.to.u32 "; break;
  case kind::STRING_TO_REGEXP: out << "str.to.re "; break;
  case kind::REGEXP_CONCAT: out << "re.++ "; break;
  case kind::REGEXP_UNION: out << "re.union "; break;
  case kind::REGEXP_INTER: out << "re.inter "; break;
  case kind::REGEXP_STAR: out << "re.* "; break;
  case kind::REGEXP_PLUS: out << "re.+ "; break;
  case kind::REGEXP_OPT: out << "re.opt "; break;
  case kind::REGEXP_RANGE: out << "re.range "; break;
  case kind::REGEXP_LOOP: out << "re.loop "; break;
  case kind::REGEXP_EMPTY: out << "re.nostr "; break;
  case kind::REGEXP_SIGMA: out << "re.allchar "; break;

    // bv theory
  case kind::BITVECTOR_CONCAT: out << "concat "; forceBinary = true; break;
  case kind::BITVECTOR_AND: out << "bvand "; forceBinary = true; break;
  case kind::BITVECTOR_OR: out << "bvor "; forceBinary = true; break;
  case kind::BITVECTOR_XOR: out << "bvxor "; forceBinary = true; break;
  case kind::BITVECTOR_NOT: out << "bvnot "; break;
  case kind::BITVECTOR_NAND: out << "bvnand "; break;
  case kind::BITVECTOR_NOR: out << "bvnor "; break;
  case kind::BITVECTOR_XNOR: out << "bvxnor "; break;
  case kind::BITVECTOR_COMP: out << "bvcomp "; break;
  case kind::BITVECTOR_MULT: out << "bvmul "; forceBinary = true; break;
  case kind::BITVECTOR_PLUS: out << "bvadd "; forceBinary = true; break;
  case kind::BITVECTOR_SUB: out << "bvsub "; break;
  case kind::BITVECTOR_NEG: out << "bvneg "; break;
  case kind::BITVECTOR_UDIV: out << "bvudiv "; break;
  case kind::BITVECTOR_UDIV_TOTAL: out << "bvudiv_total "; break;
  case kind::BITVECTOR_UREM: out << "bvurem "; break;
  case kind::BITVECTOR_UREM_TOTAL: out << "bvurem_total "; break;
  case kind::BITVECTOR_SDIV: out << "bvsdiv "; break;
  case kind::BITVECTOR_SREM: out << "bvsrem "; break;
  case kind::BITVECTOR_SMOD: out << "bvsmod "; break;
  case kind::BITVECTOR_SHL: out << "bvshl "; break;
  case kind::BITVECTOR_LSHR: out << "bvlshr "; break;
  case kind::BITVECTOR_ASHR: out << "bvashr "; break;
  case kind::BITVECTOR_ULT: out << "bvult "; break;
  case kind::BITVECTOR_ULE: out << "bvule "; break;
  case kind::BITVECTOR_UGT: out << "bvugt "; break;
  case kind::BITVECTOR_UGE: out << "bvuge "; break;
  case kind::BITVECTOR_SLT: out << "bvslt "; break;
  case kind::BITVECTOR_SLE: out << "bvsle "; break;
  case kind::BITVECTOR_SGT: out << "bvsgt "; break;
  case kind::BITVECTOR_SGE: out << "bvsge "; break;
  case kind::BITVECTOR_TO_NAT: out << "bv2nat "; break;

  case kind::BITVECTOR_EXTRACT:
  case kind::BITVECTOR_REPEAT:
  case kind::BITVECTOR_ZERO_EXTEND:
  case kind::BITVECTOR_SIGN_EXTEND:
  case kind::BITVECTOR_ROTATE_LEFT:
  case kind::BITVECTOR_ROTATE_RIGHT:
  case kind::INT_TO_BITVECTOR:
    printBvParameterizedOp(out, n);
    out << ' ';
    stillNeedToPrintParams = false;
    break;

    // sets
  case kind::UNION:
  case kind::INTERSECTION:
  case kind::SETMINUS:
  case kind::SUBSET:
  case kind::MEMBER:
  case kind::SET_TYPE:
  case kind::SINGLETON: out << smtKindString(k) << " "; break;

    // fp theory
  case kind::FLOATINGPOINT_FP:
  case kind::FLOATINGPOINT_EQ:
  case kind::FLOATINGPOINT_ABS:
  case kind::FLOATINGPOINT_NEG:
  case kind::FLOATINGPOINT_PLUS:
  case kind::FLOATINGPOINT_SUB:
  case kind::FLOATINGPOINT_MULT:
  case kind::FLOATINGPOINT_DIV:
  case kind::FLOATINGPOINT_FMA:
  case kind::FLOATINGPOINT_SQRT:
  case kind::FLOATINGPOINT_REM:
  case kind::FLOATINGPOINT_RTI:
  case kind::FLOATINGPOINT_MIN:
  case kind::FLOATINGPOINT_MAX:
  case kind::FLOATINGPOINT_LEQ:
  case kind::FLOATINGPOINT_LT:
  case kind::FLOATINGPOINT_GEQ:
  case kind::FLOATINGPOINT_GT:
  case kind::FLOATINGPOINT_ISN:
  case kind::FLOATINGPOINT_ISSN:
  case kind::FLOATINGPOINT_ISZ:
  case kind::FLOATINGPOINT_ISINF:
  case kind::FLOATINGPOINT_ISNAN:
  case kind::FLOATINGPOINT_ISNEG:
  case kind::FLOATINGPOINT_ISPOS:
  case kind::FLOATINGPOINT_TO_REAL:
    out << smtKindString(k) << ' '; break;

  case kind::FLOATINGPOINT_TO_FP_IEEE_BITVECTOR:
  case kind::FLOATINGPOINT_TO_FP_FLOATINGPOINT:
  case kind::FLOATINGPOINT_TO_FP_REAL:
  case kind::FLOATINGPOINT_TO_FP_SIGNED_BITVECTOR:
  case kind::FLOATINGPOINT_TO_FP_UNSIGNED_BITVECTOR:
  case kind::FLOATINGPOINT_TO_FP_GENERIC:
  case kind::FLOATINGPOINT_TO_UBV:
  case kind::FLOATINGPOINT_TO_SBV:
    printFpParameterizedOp(out, n);
    out << ' ';
    stillNeedToPrintParams = false;
    break;

    // datatypes
  case kind::APPLY_TYPE_ASCRIPTION: {
      TypeNode t = TypeNode::fromType(n.getOperator().getConst<AscriptionType>().getType());
      if(t.getKind() == kind::TYPE_CONSTANT &&
         t.getConst<TypeConstant>() == REAL_TYPE &&
         n[0].getType().isInteger()) {
        // Special case, in model output integer constants that should be
        // Real-sorted are wrapped in a type ascription.  Handle that here.

	// Note: This is Tim making a guess about Morgan's Code.
	Assert(n[0].getKind() == kind::CONST_RATIONAL);	
	toStreamRational(out, n[0].getConst<Rational>(), true);

        //toStream(out, n[0], -1, false);
        //out << ".0";
	
        return;
      }
      out << "(as ";
      toStream(out, n[0], toDepth < 0 ? toDepth : toDepth - 1, types);
      out << ' ' << (t.isFunctionLike() ? t.getRangeType() : t) << ')';
      return;
    }
    break;
  case kind::APPLY_TESTER:
  case kind::APPLY_CONSTRUCTOR:
  case kind::APPLY_SELECTOR:
  case kind::APPLY_SELECTOR_TOTAL:
  case kind::PARAMETRIC_DATATYPE:
    break;

    // quantifiers
  case kind::FORALL:
  case kind::EXISTS:
    if( k==kind::FORALL ){
      out << "forall ";
    }else{
      out << "exists ";
    }
    for( unsigned i=0; i<2; i++) {
      out << n[i] << " ";
      if( i==0 && n.getNumChildren()==3 ){
        out << "(! ";
      }
    }
    if( n.getNumChildren()==3 ){
      out << n[2];
      out << ")";
    }
    out << ")";
    return;
    break;
  case kind::BOUND_VAR_LIST:
    // the left parenthesis is already printed (before the switch)
    for(TNode::iterator i = n.begin(), iend = n.end();
        i != iend; ) {
      out << '(';
      toStream(out, *i, toDepth < 0 ? toDepth : toDepth - 1, types, 0);
      out << ' ';
      out << (*i).getType();
      // The following code do stange things
      // (*i).getType().toStream(out, toDepth < 0 ? toDepth : toDepth - 1,
      //                         false, language::output::LANG_SMTLIB_V2_5);
      out << ')';
      if(++i != iend) {
        out << ' ';
      }
    }
    out << ')';
    return;
  case kind::INST_PATTERN:
    break;
  case kind::INST_PATTERN_LIST:
    for(unsigned i=0; i<n.getNumChildren(); i++) {
      if( n[i].getKind()==kind::INST_ATTRIBUTE ){
        if( n[i][0].getAttribute(theory::FunDefAttribute()) ){
          out << ":fun-def";
        }
      }else{
        out << ":pattern " << n[i];
      }
    }
    return;
    break;

  default:
    // fall back on however the kind prints itself; this probably
    // won't be SMT-LIB v2 compliant, but it will be clear from the
    // output that support for the kind needs to be added here.
    out << n.getKind() << ' ';
  }
  if( n.getMetaKind() == kind::metakind::PARAMETERIZED &&
      stillNeedToPrintParams ) {
    if(toDepth != 0) {
      toStream(out, n.getOperator(), toDepth < 0 ? toDepth : toDepth - 1, types);
    } else {
      out << "(...)";
    }
    if(n.getNumChildren() > 0) {
      out << ' ';
    }
  }
  stringstream parens;
  for(size_t i = 0, c = 1; i < n.getNumChildren(); ) {
    if(toDepth != 0) {
      toStream(out, n[i], toDepth < 0 ? toDepth : toDepth - c, types);
    } else {
      out << "(...)";
    }
    if(++i < n.getNumChildren()) {
      if(forceBinary && i < n.getNumChildren() - 1) {
        // not going to work properly for parameterized kinds!
        Assert(n.getMetaKind() != kind::metakind::PARAMETERIZED);
        out << " (" << smtKindString(n.getKind()) << ' ';
        parens << ')';
        ++c;
      } else {
        out << ' ';
      }
    }
  }
  if(n.getNumChildren() != 0) {
    out << parens.str() << ')';
  }
}/* Smt2Printer::toStream(TNode) */

static string smtKindString(Kind k) throw() {
  switch(k) {
    // builtin theory
  case kind::APPLY: break;
  case kind::EQUAL: return "=";
  case kind::DISTINCT: return "distinct";
  case kind::CHAIN: break;
  case kind::TUPLE: break;
  case kind::SEXPR: break;

    // bool theory
  case kind::NOT: return "not";
  case kind::AND: return "and";
  case kind::IFF: return "=";
  case kind::IMPLIES: return "=>";
  case kind::OR: return "or";
  case kind::XOR: return "xor";
  case kind::ITE: return "ite";

    // uf theory
  case kind::APPLY_UF: break;

    // arith theory
  case kind::PLUS: return "+";
  case kind::MULT: return "*";
  case kind::MINUS: return "-";
  case kind::UMINUS: return "-";
  case kind::LT: return "<";
  case kind::LEQ: return "<=";
  case kind::GT: return ">";
  case kind::GEQ: return ">=";
  case kind::DIVISION:
  case kind::DIVISION_TOTAL: return "/";
  case kind::INTS_DIVISION: return "div";
  case kind::INTS_DIVISION_TOTAL: return "INTS_DIVISION_TOTAL";
  case kind::INTS_MODULUS: return "mod";
  case kind::INTS_MODULUS_TOTAL: return "INTS_MODULUS_TOTAL";
  case kind::ABS: return "abs";
  case kind::IS_INTEGER: return "is_int";
  case kind::TO_INTEGER: return "to_int";
  case kind::TO_REAL: return "to_real";
  case kind::POW: return "^";

    // arrays theory
  case kind::SELECT: return "select";
  case kind::STORE: return "store";
  case kind::ARRAY_TYPE: return "Array";

    // bv theory
  case kind::BITVECTOR_CONCAT: return "concat";
  case kind::BITVECTOR_AND: return "bvand";
  case kind::BITVECTOR_OR: return "bvor";
  case kind::BITVECTOR_XOR: return "bvxor";
  case kind::BITVECTOR_NOT: return "bvnot";
  case kind::BITVECTOR_NAND: return "bvnand";
  case kind::BITVECTOR_NOR: return "bvnor";
  case kind::BITVECTOR_XNOR: return "bvxnor";
  case kind::BITVECTOR_COMP: return "bvcomp";
  case kind::BITVECTOR_MULT: return "bvmul";
  case kind::BITVECTOR_PLUS: return "bvadd";
  case kind::BITVECTOR_SUB: return "bvsub";
  case kind::BITVECTOR_NEG: return "bvneg";
  case kind::BITVECTOR_UDIV: return "bvudiv";
  case kind::BITVECTOR_UREM: return "bvurem";
  case kind::BITVECTOR_SDIV: return "bvsdiv";
  case kind::BITVECTOR_SREM: return "bvsrem";
  case kind::BITVECTOR_SMOD: return "bvsmod";
  case kind::BITVECTOR_SHL: return "bvshl";
  case kind::BITVECTOR_LSHR: return "bvlshr";
  case kind::BITVECTOR_ASHR: return "bvashr";
  case kind::BITVECTOR_ULT: return "bvult";
  case kind::BITVECTOR_ULE: return "bvule";
  case kind::BITVECTOR_UGT: return "bvugt";
  case kind::BITVECTOR_UGE: return "bvuge";
  case kind::BITVECTOR_SLT: return "bvslt";
  case kind::BITVECTOR_SLE: return "bvsle";
  case kind::BITVECTOR_SGT: return "bvsgt";
  case kind::BITVECTOR_SGE: return "bvsge";

  case kind::BITVECTOR_EXTRACT: return "extract";
  case kind::BITVECTOR_REPEAT: return "repeat";
  case kind::BITVECTOR_ZERO_EXTEND: return "zero_extend";
  case kind::BITVECTOR_SIGN_EXTEND: return "sign_extend";
  case kind::BITVECTOR_ROTATE_LEFT: return "rotate_left";
  case kind::BITVECTOR_ROTATE_RIGHT: return "rotate_right";

  case kind::UNION: return "union";
  case kind::INTERSECTION: return "intersection";
  case kind::SETMINUS: return "setminus";
  case kind::SUBSET: return "subset";
  case kind::MEMBER: return "member";
  case kind::SET_TYPE: return "Set";
  case kind::SINGLETON: return "singleton";
  case kind::INSERT: return "insert";

    // fp theory
  case kind::FLOATINGPOINT_FP: return "fp";
  case kind::FLOATINGPOINT_EQ: return "fp.eq";
  case kind::FLOATINGPOINT_ABS: return "fp.abs";
  case kind::FLOATINGPOINT_NEG: return "fp.neg";
  case kind::FLOATINGPOINT_PLUS: return "fp.add";
  case kind::FLOATINGPOINT_SUB: return "fp.sub";
  case kind::FLOATINGPOINT_MULT: return "fp.mul";
  case kind::FLOATINGPOINT_DIV: return "fp.div";
  case kind::FLOATINGPOINT_FMA: return "fp.fma";
  case kind::FLOATINGPOINT_SQRT: return "fp.sqrt";
  case kind::FLOATINGPOINT_REM: return "fp.rem";
  case kind::FLOATINGPOINT_RTI: return "fp.roundToIntegral";
  case kind::FLOATINGPOINT_MIN: return "fp.min";
  case kind::FLOATINGPOINT_MAX: return "fp.max";

  case kind::FLOATINGPOINT_LEQ: return "fp.leq";
  case kind::FLOATINGPOINT_LT: return "fp.lt";
  case kind::FLOATINGPOINT_GEQ: return "fp.geq";
  case kind::FLOATINGPOINT_GT: return "fp.gt";

  case kind::FLOATINGPOINT_ISN: return "fp.isNormal";
  case kind::FLOATINGPOINT_ISSN: return "fp.isSubnormal";
  case kind::FLOATINGPOINT_ISZ: return "fp.isZero";  
  case kind::FLOATINGPOINT_ISINF: return "fp.isInfinite";
  case kind::FLOATINGPOINT_ISNAN: return "fp.isNaN";
  case kind::FLOATINGPOINT_ISNEG: return "fp.isNegative";
  case kind::FLOATINGPOINT_ISPOS: return "fp.isPositive";

  case kind::FLOATINGPOINT_TO_FP_IEEE_BITVECTOR: return "to_fp";
  case kind::FLOATINGPOINT_TO_FP_FLOATINGPOINT: return "to_fp";
  case kind::FLOATINGPOINT_TO_FP_REAL: return "to_fp";
  case kind::FLOATINGPOINT_TO_FP_SIGNED_BITVECTOR: return "to_fp";
  case kind::FLOATINGPOINT_TO_FP_UNSIGNED_BITVECTOR: return "to_fp_unsigned";
  case kind::FLOATINGPOINT_TO_FP_GENERIC: return "to_fp_unsigned";
  case kind::FLOATINGPOINT_TO_UBV: return "fp.to_ubv";
  case kind::FLOATINGPOINT_TO_SBV: return "fp.to_sbv";
  case kind::FLOATINGPOINT_TO_REAL: return "fp.to_real";

  default:
    ; /* fall through */
  }

  // no SMT way to print these
  return kind::kindToString(k);
}

static void printBvParameterizedOp(std::ostream& out, TNode n) throw() {
  out << "(_ ";
  switch(n.getKind()) {
  case kind::BITVECTOR_EXTRACT: {
    BitVectorExtract p = n.getOperator().getConst<BitVectorExtract>();
    out << "extract " << p.high << ' ' << p.low;
    break;
  }
  case kind::BITVECTOR_REPEAT:
    out << "repeat "
        << n.getOperator().getConst<BitVectorRepeat>().repeatAmount;
    break;
  case kind::BITVECTOR_ZERO_EXTEND:
    out << "zero_extend "
        << n.getOperator().getConst<BitVectorZeroExtend>().zeroExtendAmount;
    break;
  case kind::BITVECTOR_SIGN_EXTEND:
    out << "sign_extend "
        << n.getOperator().getConst<BitVectorSignExtend>().signExtendAmount;
    break;
  case kind::BITVECTOR_ROTATE_LEFT:
    out << "rotate_left "
        << n.getOperator().getConst<BitVectorRotateLeft>().rotateLeftAmount;
    break;
  case kind::BITVECTOR_ROTATE_RIGHT:
    out << "rotate_right "
        << n.getOperator().getConst<BitVectorRotateRight>().rotateRightAmount;
    break;
  case kind::INT_TO_BITVECTOR:
    out << "int2bv "
        << n.getOperator().getConst<IntToBitVector>().size;
    break;
  default:
    out << n.getKind();
  }
  out << ")";
}

static void printFpParameterizedOp(std::ostream& out, TNode n) throw() {
  out << "(_ ";
  switch(n.getKind()) {
  case kind::FLOATINGPOINT_TO_FP_IEEE_BITVECTOR:
    //out << "to_fp_bv "
    out << "to_fp "
        << n.getOperator().getConst<FloatingPointToFPIEEEBitVector>().t.exponent() << ' '
        << n.getOperator().getConst<FloatingPointToFPIEEEBitVector>().t.significand();
    break;
  case kind::FLOATINGPOINT_TO_FP_FLOATINGPOINT:
    //out << "to_fp_fp "
    out << "to_fp "
        << n.getOperator().getConst<FloatingPointToFPFloatingPoint>().t.exponent() << ' '
        << n.getOperator().getConst<FloatingPointToFPFloatingPoint>().t.significand();
    break;
  case kind::FLOATINGPOINT_TO_FP_REAL:
    //out << "to_fp_real "
    out << "to_fp "
        << n.getOperator().getConst<FloatingPointToFPReal>().t.exponent() << ' '
        << n.getOperator().getConst<FloatingPointToFPReal>().t.significand();
    break;
  case kind::FLOATINGPOINT_TO_FP_SIGNED_BITVECTOR:
    //out << "to_fp_signed "
    out << "to_fp "
        << n.getOperator().getConst<FloatingPointToFPSignedBitVector>().t.exponent() << ' '
        << n.getOperator().getConst<FloatingPointToFPSignedBitVector>().t.significand();
    break;
  case kind::FLOATINGPOINT_TO_FP_UNSIGNED_BITVECTOR:
    out << "to_fp_unsigned "
        << n.getOperator().getConst<FloatingPointToFPUnsignedBitVector>().t.exponent() << ' '
        << n.getOperator().getConst<FloatingPointToFPUnsignedBitVector>().t.significand();
    break;
  case kind::FLOATINGPOINT_TO_FP_GENERIC:
    out << "to_fp "
        << n.getOperator().getConst<FloatingPointToFPGeneric>().t.exponent() << ' '
        << n.getOperator().getConst<FloatingPointToFPGeneric>().t.significand();
    break;
  case kind::FLOATINGPOINT_TO_UBV:
    out << "fp.to_ubv "
        << n.getOperator().getConst<FloatingPointToUBV>().bvs.size;
    break;
  case kind::FLOATINGPOINT_TO_SBV:
    out << "fp.to_sbv "
        << n.getOperator().getConst<FloatingPointToSBV>().bvs.size;
    break;
  default:
    out << n.getKind();
  }
  out << ")";
}


template <class T>
static bool tryToStream(std::ostream& out, const Command* c) throw();
template <class T>
static bool tryToStream(std::ostream& out, const Command* c, Variant v) throw();

void Smt2Printer::toStream(std::ostream& out, const Command* c,
                           int toDepth, bool types, size_t dag) const throw() {
  expr::ExprSetDepth::Scope sdScope(out, toDepth);
  expr::ExprPrintTypes::Scope ptScope(out, types);
  expr::ExprDag::Scope dagScope(out, dag);

  if(tryToStream<AssertCommand>(out, c) ||
     tryToStream<PushCommand>(out, c) ||
     tryToStream<PopCommand>(out, c) ||
     tryToStream<CheckSatCommand>(out, c) ||
     tryToStream<QueryCommand>(out, c) ||
     tryToStream<ResetCommand>(out, c) ||
     tryToStream<ResetAssertionsCommand>(out, c) ||
     tryToStream<QuitCommand>(out, c) ||
     tryToStream<DeclarationSequence>(out, c) ||
     tryToStream<CommandSequence>(out, c) ||
     tryToStream<DeclareFunctionCommand>(out, c) ||
     tryToStream<DeclareTypeCommand>(out, c) ||
     tryToStream<DefineTypeCommand>(out, c) ||
     tryToStream<DefineNamedFunctionCommand>(out, c) ||
     tryToStream<DefineFunctionCommand>(out, c) ||
     tryToStream<SimplifyCommand>(out, c) ||
     tryToStream<GetValueCommand>(out, c) ||
     tryToStream<GetModelCommand>(out, c) ||
     tryToStream<GetAssignmentCommand>(out, c) ||
     tryToStream<GetAssertionsCommand>(out, c) ||
     tryToStream<GetProofCommand>(out, c) ||
     tryToStream<GetUnsatCoreCommand>(out, c) ||
     tryToStream<SetBenchmarkStatusCommand>(out, c, d_variant) ||
     tryToStream<SetBenchmarkLogicCommand>(out, c, d_variant) ||
     tryToStream<SetInfoCommand>(out, c, d_variant) ||
     tryToStream<GetInfoCommand>(out, c) ||
     tryToStream<SetOptionCommand>(out, c) ||
     tryToStream<GetOptionCommand>(out, c) ||
     tryToStream<DatatypeDeclarationCommand>(out, c) ||
     tryToStream<CommentCommand>(out, c, d_variant) ||
     tryToStream<EmptyCommand>(out, c) ||
     tryToStream<EchoCommand>(out, c, d_variant)) {
    return;
  }

  out << "ERROR: don't know how to print a Command of class: "
      << typeid(*c).name() << endl;

}/* Smt2Printer::toStream(Command*) */

static inline void toStream(std::ostream& out, const SExpr& sexpr) throw() {
  Printer::getPrinter(language::output::LANG_SMTLIB_V2_5)->toStream(out, sexpr);
}

// SMT-LIB quoting for symbols
static std::string quoteSymbol(std::string s) {
  if(s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789~!@$%^&*_-+=<>.?/") == std::string::npos) {
    // simple unquoted symbol
    return s;
  }

  // must quote the symbol, but it cannot contain | or \, we turn those into _
  size_t p;
  while((p = s.find_first_of("\\|")) != std::string::npos) {
    s = s.replace(p, 1, "_");
  }
  return "|" + s + "|";
}

static std::string quoteSymbol(TNode n) {
  std::stringstream ss;
  ss << Expr::setlanguage(language::output::LANG_SMTLIB_V2_5);
  return quoteSymbol(ss.str());
}

void Smt2Printer::toStream(std::ostream& out, const SExpr& sexpr) const throw() {
  if(sexpr.isKeyword()) {
    out << quoteSymbol(sexpr.getValue());
  } else {
    this->Printer::toStream(out, sexpr);
  }
}

template <class T>
static bool tryToStream(std::ostream& out, const CommandStatus* s, Variant v) throw();

void Smt2Printer::toStream(std::ostream& out, const CommandStatus* s) const throw() {

  if(tryToStream<CommandSuccess>(out, s, d_variant) ||
     tryToStream<CommandFailure>(out, s, d_variant) ||
     tryToStream<CommandUnsupported>(out, s, d_variant) ||
     tryToStream<CommandInterrupted>(out, s, d_variant)) {
    return;
  }

  out << "ERROR: don't know how to print a CommandStatus of class: "
      << typeid(*s).name() << endl;

}/* Smt2Printer::toStream(CommandStatus*) */


void Smt2Printer::toStream(std::ostream& out, const UnsatCore& core, const std::map<Expr, std::string>& names) const throw() {
  out << "(" << std::endl;
  for(UnsatCore::const_iterator i = core.begin(); i != core.end(); ++i) {
    map<Expr, string>::const_iterator j = names.find(*i);
    if(j == names.end()) {
      out << *i << endl;
    } else {
      out << (*j).second << endl;
    }
  }
  out << ")" << endl;
}/* Smt2Printer::toStream(UnsatCore, map<Expr, string>) */


void Smt2Printer::toStream(std::ostream& out, const Model& m) const throw() {
  out << "(model" << endl;
  this->Printer::toStream(out, m);
  out << ")" << endl;
}


void Smt2Printer::toStream(std::ostream& out, const Model& m, const Command* c) const throw() {
  const theory::TheoryModel& tm = (const theory::TheoryModel&) m;
  if(dynamic_cast<const DeclareTypeCommand*>(c) != NULL) {
    TypeNode tn = TypeNode::fromType( ((const DeclareTypeCommand*)c)->getType() );
    if( options::modelUninterpDtEnum() && tn.isSort() &&
        tm.d_rep_set.d_type_reps.find( tn )!=tm.d_rep_set.d_type_reps.end() ){
      out << "(declare-datatypes () ((" << dynamic_cast<const DeclareTypeCommand*>(c)->getSymbol() << " ";
      for( size_t i=0; i<(*tm.d_rep_set.d_type_reps.find(tn)).second.size(); i++ ){
        out << "(" << (*tm.d_rep_set.d_type_reps.find(tn)).second[i] << ")";
      }
      out << ")))" << endl;
    } else {
      if( tn.isSort() ){
        //print the cardinality
        if( tm.d_rep_set.d_type_reps.find( tn )!=tm.d_rep_set.d_type_reps.end() ){
          out << "; cardinality of " << tn << " is " << (*tm.d_rep_set.d_type_reps.find(tn)).second.size() << endl;
        }
      }
      out << c << endl;
      if( tn.isSort() ){
        //print the representatives
        if( tm.d_rep_set.d_type_reps.find( tn )!=tm.d_rep_set.d_type_reps.end() ){
          for( size_t i=0; i<(*tm.d_rep_set.d_type_reps.find(tn)).second.size(); i++ ){
            if( (*tm.d_rep_set.d_type_reps.find(tn)).second[i].isVar() ){
              out << "(declare-fun " << quoteSymbol((*tm.d_rep_set.d_type_reps.find(tn)).second[i]) << " () " << tn << ")" << endl;
            }else{
              out << "; rep: " << (*tm.d_rep_set.d_type_reps.find(tn)).second[i] << endl;
            }
          }
        }
      }
    }
  } else if(dynamic_cast<const DeclareFunctionCommand*>(c) != NULL) {
    const DeclareFunctionCommand* dfc = (const DeclareFunctionCommand*)c; 
    Node n = Node::fromExpr( dfc->getFunction() );
    if(dfc->getPrintInModelSetByUser()) {
      if(!dfc->getPrintInModel()) {
        return;
      }
    } else if(n.getKind() == kind::SKOLEM) {
      // don't print out internal stuff
      return;
    }
    Node val = Node::fromExpr(tm.getSmtEngine()->getValue(n.toExpr()));
    if(val.getKind() == kind::LAMBDA) {
      out << "(define-fun " << n << " " << val[0]
          << " " << n.getType().getRangeType()
          << " " << val[1] << ")" << endl;
    } else {
      if( options::modelUninterpDtEnum() && val.getKind() == kind::STORE ) {
        TypeNode tn = val[1].getType();
        if (tn.isSort() && tm.d_rep_set.d_type_reps.find( tn )!=tm.d_rep_set.d_type_reps.end() ){
          Cardinality indexCard((*tm.d_rep_set.d_type_reps.find(tn)).second.size());
          val = theory::arrays::TheoryArraysRewriter::normalizeConstant( val, indexCard );
        }
      }
      out << "(define-fun " << n << " () "
          << n.getType() << " ";
      if(val.getType().isInteger() && n.getType().isReal() && !n.getType().isInteger()) {
	//toStreamReal(out, val, true);
	toStreamRational(out, val.getConst<Rational>(), true);
	//out << val << ".0";	
      } else {
        out << val;
      }
      out << ")" << endl;
    }
/*
    //for table format (work in progress)
    bool printedModel = false;
    if( tn.isFunction() ){
      if( options::modelFormatMode()==MODEL_FORMAT_MODE_TABLE ){
        //specialized table format for functions
        RepSetIterator riter( &d_rep_set );
        riter.setFunctionDomain( n );
        while( !riter.isFinished() ){
          std::vector< Node > children;
          children.push_back( n );
          for( int i=0; i<riter.getNumTerms(); i++ ){
            children.push_back( riter.getTerm( i ) );
          }
          Node nn = NodeManager::currentNM()->mkNode( APPLY_UF, children );
          Node val = getValue( nn );
          out << val << " ";
          riter.increment();
        }
        printedModel = true;
      }
    }
*/
  } else {
    out << c << endl;
  }
}

void Smt2Printer::toStream(std::ostream& out, const Result& r) const throw() {
  if(r.getType() == Result::TYPE_SAT && r.isSat() == Result::SAT_UNKNOWN) {
    out << "unknown";
  } else {
    Printer::toStream(out, r);
  }
}

static void toStream(std::ostream& out, const AssertCommand* c) throw() {
  out << "(assert " << c->getExpr() << ")";
}

static void toStream(std::ostream& out, const PushCommand* c) throw() {
  out << "(push 1)";
}

static void toStream(std::ostream& out, const PopCommand* c) throw() {
  out << "(pop 1)";
}

static void toStream(std::ostream& out, const CheckSatCommand* c) throw() {
  Expr e = c->getExpr();
  if(!e.isNull() && !(e.getKind() == kind::CONST_BOOLEAN && e.getConst<bool>())) {
    out << PushCommand() << endl
        << AssertCommand(e) << endl
        << CheckSatCommand() << endl
        << PopCommand();
  } else {
    out << "(check-sat)";
  }
}

static void toStream(std::ostream& out, const QueryCommand* c) throw() {
  Expr e = c->getExpr();
  if(!e.isNull()) {
    out << PushCommand() << endl
        << AssertCommand(BooleanSimplification::negate(e)) << endl
        << CheckSatCommand() << endl
        << PopCommand();
  } else {
    out << "(check-sat)";
  }
}

static void toStream(std::ostream& out, const ResetCommand* c) throw() {
  out << "(reset)";
}

static void toStream(std::ostream& out, const ResetAssertionsCommand* c) throw() {
  out << "(reset-assertions)";
}

static void toStream(std::ostream& out, const QuitCommand* c) throw() {
  out << "(exit)";
}

static void toStream(std::ostream& out, const CommandSequence* c) throw() {
  CommandSequence::const_iterator i = c->begin();
  if(i != c->end()) {
    for(;;) {
      out << *i;
      if(++i != c->end()) {
        out << endl;
      } else {
        break;
      }
    }
  }
}

static void toStream(std::ostream& out, const DeclareFunctionCommand* c) throw() {
  Type type = c->getType();
  out << "(declare-fun " << quoteSymbol(c->getSymbol()) << " (";
  if(type.isFunction()) {
    FunctionType ft = type;
    const vector<Type> argTypes = ft.getArgTypes();
    if(argTypes.size() > 0) {
      copy( argTypes.begin(), argTypes.end() - 1,
            ostream_iterator<Type>(out, " ") );
      out << argTypes.back();
    }
    type = ft.getRangeType();
  }

  out << ") " << type << ")";
}

static void toStream(std::ostream& out, const DefineFunctionCommand* c) throw() {
  Expr func = c->getFunction();
  const vector<Expr>* formals = &c->getFormals();
  out << "(define-fun " << func << " (";
  Type type = func.getType();
  Expr formula = c->getFormula();
  if(type.isFunction()) {
    vector<Expr> f;
    if(formals->empty()) {
      const vector<Type>& params = FunctionType(type).getArgTypes();
      for(vector<Type>::const_iterator j = params.begin(); j != params.end(); ++j) {
        f.push_back(NodeManager::currentNM()->mkSkolem("a", TypeNode::fromType(*j), "",
                                                       NodeManager::SKOLEM_NO_NOTIFY).toExpr());
      }
      formula = NodeManager::currentNM()->toExprManager()->mkExpr(kind::APPLY_UF, formula, f);
      formals = &f;
    }
    vector<Expr>::const_iterator i = formals->begin();
    for(;;) {
      out << "(" << (*i) << " " << (*i).getType() << ")";
      ++i;
      if(i != formals->end()) {
        out << " ";
      } else {
        break;
      }
    }
    type = FunctionType(type).getRangeType();
  }
  out << ") " << type << " " << formula << ")";
}

static void toStreamRational(std::ostream& out, const Rational& r, bool decimal) throw() {
  bool neg = r.sgn() < 0;
  
  // TODO:
  // We are currently printing (- (/ 5 3))
  // instead of (/ (- 5) 3) which is what is in the SMT-LIB value in the theory definition.
  // Before switching, I'll keep to what was there and send an email.

  // Tim: Apologies for the ifs on one line but in this case they are cleaner.
  
  if (neg) { out << "(- "; }
  
  if(r.isIntegral()) {
    if (neg) {
      out << (-r);
    }else {
      out << r;
    }
    if (decimal) { out << ".0"; }
  }else{
    out << "(/ ";
    if(neg) {
      Rational abs_r = (-r);
      out << abs_r.getNumerator();
      if(decimal) { out << ".0"; }
      out << ' ' << abs_r.getDenominator();
      if(decimal) { out << ".0"; }
    }else{
      out << r.getNumerator();
      if(decimal) { out << ".0"; }
      out << ' ' << r.getDenominator();
      if(decimal) { out << ".0"; }
    }
    out << ')';
  }

  if (neg) { out << ')';}

  // if(r < 0) {
  //   Rational abs_r = -r;
  //   if(r.isIntegral()) {
  //     out << "(- " << abs_r << ')';
  //   } else {
  //     out << "(- (/ " << (-r).getNumerator() << ' ' << (-r).getDenominator() << "))";
  //   }
  // } else {
  //   if(r.isIntegral()) {
  //         out << r;
  //       } else {
  //         out << "(/ " << r.getNumerator() << ' ' << r.getDenominator() << ')';
  //       }
  //     }
}


static void toStream(std::ostream& out, const DeclareTypeCommand* c) throw() {
  out << "(declare-sort " << c->getSymbol() << " " << c->getArity() << ")";
}

static void toStream(std::ostream& out, const DefineTypeCommand* c) throw() {
  const vector<Type>& params = c->getParameters();
  out << "(define-sort " << c->getSymbol() << " (";
  if(params.size() > 0) {
    copy( params.begin(), params.end() - 1,
          ostream_iterator<Type>(out, " ") );
    out << params.back();
  }
  out << ") " << c->getType() << ")";
}

static void toStream(std::ostream& out, const DefineNamedFunctionCommand* c) throw() {
  out << "DefineNamedFunction( ";
  toStream(out, static_cast<const DefineFunctionCommand*>(c));
  out << " )";

  out << "ERROR: don't know how to output define-named-function command" << endl;
}

static void toStream(std::ostream& out, const SimplifyCommand* c) throw() {
  out << "(simplify " << c->getTerm() << ")";
}

static void toStream(std::ostream& out, const GetValueCommand* c) throw() {
  out << "(get-value ( ";
  const vector<Expr>& terms = c->getTerms();
  copy(terms.begin(), terms.end(), ostream_iterator<Expr>(out, " "));
  out << "))";
}

static void toStream(std::ostream& out, const GetModelCommand* c) throw() {
  out << "(get-model)";
}

static void toStream(std::ostream& out, const GetAssignmentCommand* c) throw() {
  out << "(get-assignment)";
}

static void toStream(std::ostream& out, const GetAssertionsCommand* c) throw() {
  out << "(get-assertions)";
}

static void toStream(std::ostream& out, const GetProofCommand* c) throw() {
  out << "(get-proof)";
}

static void toStream(std::ostream& out, const GetUnsatCoreCommand* c) throw() {
  out << "(get-unsat-core)";
}

static void toStream(std::ostream& out, const SetBenchmarkStatusCommand* c, Variant v) throw() {
  if(v == z3str_variant || v == smt2_0_variant) {
    out << "(set-info :status " << c->getStatus() << ")";
  } else {
    out << "(meta-info :status " << c->getStatus() << ")";
  }
}

static void toStream(std::ostream& out, const SetBenchmarkLogicCommand* c, Variant v) throw() {
  // Z3-str doesn't have string-specific logic strings(?), so comment it
  if(v == z3str_variant) {
    out << "; (set-logic " << c->getLogic() << ")";
  } else {
    out << "(set-logic " << c->getLogic() << ")";
  }
}

static void toStream(std::ostream& out, const SetInfoCommand* c, Variant v) throw() {
  if(v == z3str_variant || v == smt2_0_variant) {
    out << "(set-info :" << c->getFlag() << " ";
  } else {
    out << "(meta-info :" << c->getFlag() << " ";
  }
  toStream(out, c->getSExpr());
  out << ")";
}

static void toStream(std::ostream& out, const GetInfoCommand* c) throw() {
  out << "(get-info :" << c->getFlag() << ")";
}

static void toStream(std::ostream& out, const SetOptionCommand* c) throw() {
  out << "(set-option :" << c->getFlag() << " ";
  toStream(out, c->getSExpr());
  out << ")";
}

static void toStream(std::ostream& out, const GetOptionCommand* c) throw() {
  out << "(get-option :" << c->getFlag() << ")";
}

static void toStream(std::ostream& out, const DatatypeDeclarationCommand* c) throw() {
  const vector<DatatypeType>& datatypes = c->getDatatypes();
  out << "(declare-datatypes () (";
  for(vector<DatatypeType>::const_iterator i = datatypes.begin(),
        i_end = datatypes.end();
      i != i_end;
      ++i) {

    const Datatype & d = i->getDatatype();

    out << "(" << maybeQuoteSymbol(d.getName()) << " ";
    for(Datatype::const_iterator ctor = d.begin(), ctor_end = d.end();
        ctor != ctor_end; ++ctor){
      if( ctor!=d.begin() ) out << " ";
      out << "(" << maybeQuoteSymbol(ctor->getName());

      for(DatatypeConstructor::const_iterator arg = ctor->begin(), arg_end = ctor->end();
          arg != arg_end; ++arg){
        out << " (" << arg->getSelector() << " "
            << static_cast<SelectorType>(arg->getType()).getRangeType() << ")";
      }
      out << ")";
    }
    out << ")" << endl;
  }
  out << "))";
}

static void toStream(std::ostream& out, const CommentCommand* c, Variant v) throw() {
  string s = c->getComment();
  size_t pos = 0;
  while((pos = s.find_first_of('"', pos)) != string::npos) {
    s.replace(pos, 1, (v == z3str_variant || v == smt2_0_variant) ? "\\\"" : "\"\"");
    pos += 2;
  }
  out << "(set-info :notes \"" << s << "\")";
}

static void toStream(std::ostream& out, const EmptyCommand* c) throw() {
}

static void toStream(std::ostream& out, const EchoCommand* c, Variant v) throw() {
  std::string s = c->getOutput();
  // escape all double-quotes
  size_t pos = 0;
  while((pos = s.find('"', pos)) != string::npos) {
    s.replace(pos, 1, (v == z3str_variant || v == smt2_0_variant) ? "\\\"" : "\"\"");
    pos += 2;
  }
  out << "(echo \"" << s << "\")";
}

template <class T>
static bool tryToStream(std::ostream& out, const Command* c) throw() {
  if(typeid(*c) == typeid(T)) {
    toStream(out, dynamic_cast<const T*>(c));
    return true;
  }
  return false;
}

template <class T>
static bool tryToStream(std::ostream& out, const Command* c, Variant v) throw() {
  if(typeid(*c) == typeid(T)) {
    toStream(out, dynamic_cast<const T*>(c), v);
    return true;
  }
  return false;
}

static void toStream(std::ostream& out, const CommandSuccess* s, Variant v) throw() {
  if(Command::printsuccess::getPrintSuccess(out)) {
    out << "success" << endl;
  }
}

static void toStream(std::ostream& out, const CommandInterrupted* s, Variant v) throw() {
  out << "interrupted" << endl;
}

static void toStream(std::ostream& out, const CommandUnsupported* s, Variant v) throw() {
#ifdef CVC4_COMPETITION_MODE
  // if in competition mode, lie and say we're ok
  // (we have nothing to lose by saying success, and everything to lose
  // if we say "unsupported")
  out << "success" << endl;
#else /* CVC4_COMPETITION_MODE */
  out << "unsupported" << endl;
#endif /* CVC4_COMPETITION_MODE */
}

static void toStream(std::ostream& out, const CommandFailure* s, Variant v) throw() {
  string message = s->getMessage();
  // escape all double-quotes
  size_t pos = 0;
  while((pos = message.find('"', pos)) != string::npos) {
    message.replace(pos, 1, (v == z3str_variant || v == smt2_0_variant) ? "\\\"" : "\"\"");
    pos += 2;
  }
  out << "(error \"" << message << "\")" << endl;
}

template <class T>
static bool tryToStream(std::ostream& out, const CommandStatus* s, Variant v) throw() {
  if(typeid(*s) == typeid(T)) {
    toStream(out, dynamic_cast<const T*>(s), v);
    return true;
  }
  return false;
}

}/* CVC4::printer::smt2 namespace */
}/* CVC4::printer namespace */
}/* CVC4 namespace */
