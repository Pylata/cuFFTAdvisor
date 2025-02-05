#include "sizeOptimizer.h"

namespace cuFFTAdvisor {

const struct SizeOptimizer::Polynom SizeOptimizer::UNIT = {
    .value = 1, 0, 0, 0, 0, 0, 0};

SizeOptimizer::SizeOptimizer(CudaVersion::CudaVersion version,
                             GeneralTransform &tr, bool allowTrans)
    : version(version),
      log_2(1.0 / std::log(2)),
      log_3(1.0 / std::log(3)),
      log_5(1.0 / std::log(5)),
      log_7(1.0 / std::log(7)) {
  if (Tristate::BOTH == tr.isFloat) {
    // if user is not sure if they needs double, then they doesn't need it
    tr.isFloat = Tristate::TRUE;
  }

  if (allowTrans) {
    std::vector<GeneralTransform> transposed;
    TransformGenerator::transpose(tr, transposed);
    input.insert(input.end(), transposed.begin(), transposed.end());
  } else {
    input.push_back(tr);
  }

#ifdef DEBUG
  std::vector<GeneralTransform>::iterator it;
  for (it = input.begin(); it != input.end(); ++it) {
    it->print();
  }
#endif
}

std::vector<const Transform *> *SizeOptimizer::optimize(size_t nBest,
                                                        int maxPercIncrease,
                                                        int maxMemMB,
                                                        bool squareOnly,
                                                        bool crop, int rank) {
  std::vector<GeneralTransform> preoptimized;
  for (auto in : input) {
    std::vector<GeneralTransform> *tmp;
    if(rank == GeneralTransform::RANK_3D){
      tmp = optimizeXYZ_3D(in, nBest, maxPercIncrease, squareOnly, crop);
    }else{
      tmp = optimizeXYZ_1D_2D(in, nBest, maxPercIncrease, squareOnly, crop);
    }
    preoptimized.insert(preoptimized.end(), tmp->begin(), tmp->end());
    delete tmp;
  }
  return optimizeN(&preoptimized, maxMemMB, nBest);
}

bool SizeOptimizer::sizeSort(const Transform *l, const Transform *r) {
  if (l->N != r->N) return l->N > r->N;  // prefer bigger batches
  size_t lDims = l->X * l->Y * l->Z;
  size_t rDims = r->X * r->Y * r->Z;
  if (lDims != rDims) return lDims < rDims;
  if (l->Z != r->Z) return l->Z < r->Z;
  if (l->Y != r->Y) return l->Y < r->Y;
  return l->X < r->X;
}

bool SizeOptimizer::perfSort(const Transform *l, const Transform *r) {
  if (l->isFloat && (!r->isFloat)) return true;
  if ((!l->isFloat) && r->isFloat) return false;
  // both float or double
  if (l->isReal && (!r->isReal)) return true;
  if ((!l->isReal) && r->isReal) return false;
  // both complex or real
  if (l->isInPlace && (!r->isInPlace)) return false;
  if ((!l->isInPlace) && r->isInPlace) return true;
  // both in-place or out-of-place
  if (l->isBatched && (!r->isBatched)) return true;
  if ((!l->isBatched) && r->isBatched) return false;
  // both batched or not batched
  return sizeSort(l, r);
}

std::vector<const Transform *> *SizeOptimizer::optimizeN(
    std::vector<GeneralTransform> *transforms, size_t maxMem, size_t nBest) {
  std::vector<const Transform *> *result = new std::vector<const Transform *>();
  for (auto& gt : *transforms) {
    if (Tristate::isNot(gt.isBatched)) {
      collapse(gt, false, gt.N, maxMem, result);
    }
    if (Tristate::is(gt.isBatched)) {
      collapseBatched(gt, maxMem, result);
    }
  }
  std::sort(result->begin(), result->end(), perfSort);
  while (result->size() > nBest) {
    delete result->back();
    result->pop_back();
  }
  return result;
}

void SizeOptimizer::collapseBatched(GeneralTransform &gt, size_t maxMem,
                                    std::vector<const Transform *> *result) {
  int lastN, currentN;
  lastN = currentN = 1;
  // double the amount of processed images, till you reach the limit
  bool tryNext = true;
  while (tryNext && (currentN <= gt.N)) {
    tryNext = collapse(gt, true, currentN, maxMem, result);
    if (tryNext) {
      lastN = currentN;
      currentN *= 2;
    }
  }
  // decrease by one till you find max
  currentN = std::min(gt.N, currentN - 1);
  tryNext = true;
  while (tryNext && (currentN > lastN)) {
    tryNext = !collapse(gt, true, currentN, maxMem, result);
    currentN--;
  }
}

bool SizeOptimizer::collapse(GeneralTransform &gt, bool isBatched, size_t N,
                             size_t maxMemMB,
                             std::vector<const Transform *> *result) {
  bool updated = false;

  std::vector<const Transform *> transforms;
  TransformGenerator::generate(gt.device, gt.X, gt.Y, gt.Z, N, isBatched,
                               gt.isFloat, gt.isForward, gt.isInPlace,
                               gt.isReal, transforms);

  size_t noOfTransforms = transforms.size();
  for (size_t i = 0; i < noOfTransforms; i++) {
    const Transform *t = transforms.at(i);
    BenchmarkResult r(t);
    cuFFTAdvisor::Benchmarker::estimatePlanSize(&r);
    size_t planSize = std::max(r.planSizeEstimateB, r.planSizeEstimate2B);
    size_t totalSizeBytes = r.transform->dataSizeB + planSize;
    size_t totalMB = std::ceil(toMB(totalSizeBytes));
    if (totalMB <= maxMemMB) {
      result->push_back(t);
      updated = true;
      r.transform = NULL;  // unbind, so that transform is not destroyed
    }                      // else 't' is deleted by destructor of 'r'
  }
  return updated;
}

size_t SizeOptimizer::getMaxSize(GeneralTransform &tr, int maxPercIncrease,
        bool squareOnly, bool crop) {
  size_t maxXPow2 = std::ceil(log(tr.X) * log_2);
  size_t maxX = std::pow(2, maxXPow2);
  size_t maxYPow2 = squareOnly ? maxXPow2 : std::ceil(log(tr.Y) * log_2);
  size_t maxY = squareOnly ? maxX : std::pow(2, maxYPow2);
  size_t maxZPow2 = squareOnly ? maxXPow2 : std::ceil(log(tr.Z) * log_2);
  size_t maxZ = squareOnly? maxX : std::pow(2, maxZPow2);
  size_t afterPercInc = tr.getDimSize() * ((maxPercIncrease / 100.f) + 1);
  if (crop) tr.getDimSize(); // we cannot exceed original size
  return std::min(maxX * maxY * maxZ, afterPercInc);
}

size_t SizeOptimizer::getMinSize(GeneralTransform &tr, int maxPercDecrease, bool crop) {
  if ( ! crop) return tr.getDimSize(); // we cannot get under original size
  float afterPercInc = tr.getDimSize() * (1 - (maxPercDecrease / 100.f));
  return std::max(0.f, afterPercInc);
}

void SizeOptimizer::cutter(std::vector<Polynom>* polys, GeneralTransform &tr, bool crop, size_t nBest){
  //This function generate Polynom, sort them resize the vector
  if(crop){
    std::sort (polys->begin(), polys->end(), std::greater<Polynom>());
  }else{
    std::sort (polys->begin(), polys->end(), std::less<Polynom>());
  }
  polys->resize(nBest);
}

std::vector<GeneralTransform> *SizeOptimizer::optimizeXYZ_3D(GeneralTransform &tr,
                                                          size_t nBest,
                                                          int maxPercIncrease,
                                                          bool squareOnly,
                                                          bool crop) {

  std::vector<Polynom> *polysX = generatePolys(tr.X, tr.isFloat, crop);
  cutter(polysX, tr, crop, nBest);
  std::vector<Polynom> *polysY;
  std::vector<Polynom> *polysZ;

  if ((tr.X == tr.Y) || squareOnly) {
    polysY = polysX;
  } else {
    polysY = generatePolys(tr.Y, tr.isFloat, crop);
    cutter(polysY, tr, crop, nBest);
  }

  if ((tr.X == tr.Z) || squareOnly) {
    polysZ = polysX;
  } else if (tr.Y == tr.Z) {
    polysZ = polysY;
  } else {
    polysZ = generatePolys(tr.Z, tr.isFloat, crop);
    cutter(polysZ, tr, crop, nBest);
  }

  size_t minSize = getMinSize(tr, maxPercIncrease, crop);
  size_t maxSize = getMaxSize(tr, maxPercIncrease, squareOnly, crop);

  std::vector<GeneralTransform> *result = new std::vector<GeneralTransform>;
    size_t found = 0;
    for (auto& x : *polysX) {
      for (auto& y : *polysY) {
        if (squareOnly && (x.value != y.value)) continue;
        size_t xy = x.value * y.value;
        if (xy > maxSize)
          break;  // polynoms are sorted by size, we're already above the limit
        for (auto& z : *polysZ) {
          if (squareOnly && (x.value != z.value)) continue;
          size_t xyz = xy * z.value;
          if ((found < nBest) && (xyz >= minSize) && (xyz <= maxSize)) {
            // we can take nbest only, as others very probably won't be faster
            found++;
            GeneralTransform t((int)x.value, (int)y.value, (int)z.value, tr);
            result->push_back(t);
          }
        }
      }
    }

  if ((polysZ != polysY) && (polysZ != polysX)) {
    delete polysZ;
  }
  if (polysY != polysX) {
    delete polysY;
  }
  delete polysX;
  return result;
}

std::vector<GeneralTransform> *SizeOptimizer::optimizeXYZ_1D_2D(GeneralTransform &tr,
                                                          size_t nBest,
                                                          int maxPercIncrease,
                                                          bool squareOnly,
                                                          bool crop) {

  std::vector<Polynom> *polysX = generatePolys(tr.X, tr.isFloat, crop);
  std::vector<Polynom> *polysY;
  std::vector<Polynom> *polysZ;
  std::set<Polynom, valueComparator> *recPolysX = filterOptimal(polysX, crop);
  std::set<Polynom, valueComparator> *recPolysY;
  std::set<Polynom, valueComparator> *recPolysZ;

  if ((tr.X == tr.Y)
      || (squareOnly && (tr.Y != 1))) {
    polysY = polysX;
    recPolysY = recPolysX;
  } else {
    polysY = generatePolys(tr.Y, tr.isFloat, crop);
    recPolysY = filterOptimal(polysY, crop);
  }

  if ((tr.X == tr.Z)
	  || (squareOnly && (tr.Z != 1))) {
    polysZ = polysX;
    recPolysZ = recPolysX;
  } else if (tr.Y == tr.Z) {
    polysZ = polysY;
    recPolysZ = recPolysY;
  } else {
    polysZ = generatePolys(tr.Z, tr.isFloat, crop);
    recPolysZ = filterOptimal(polysZ, crop);
  }

  size_t minSize = getMinSize(tr, maxPercIncrease, crop);
  size_t maxSize = getMaxSize(tr, maxPercIncrease, squareOnly, crop);

  std::vector<GeneralTransform> *result = new std::vector<GeneralTransform>;
    size_t found = 0;
    for (auto& x : *recPolysX) {
      for (auto& y : *recPolysY) {
        if (squareOnly && (x.value != y.value) && (y.value != 1)) continue;
        size_t xy = x.value * y.value;
        if (xy > maxSize)
          break;  // polynoms are sorted by size, we're already above the limit
        for (auto& z : *recPolysZ) {
          if (squareOnly && (x.value != z.value) && (z.value != 1)) continue;
          size_t xyz = xy * z.value;
          if ((found < nBest) && (xyz >= minSize) && (xyz <= maxSize)) {
            // we can take nbest only, as others very probably won't be faster
            found++;
            GeneralTransform t((int)x.value, (int)y.value, (int)z.value, tr);
            result->push_back(t);
          }
        }
      }
    }

  if ((polysZ != polysY) && (polysZ != polysX)) {
    delete polysZ;
    delete recPolysZ;
  }
  if (polysY != polysX) {
    delete polysY;
    delete recPolysY;
  }
  delete polysX;
  delete recPolysX;
  return result;
}

int SizeOptimizer::getNoOfPrimes(Polynom &poly) {
  int counter = 0;
  if (poly.exponent2 != 0) counter++;
  if (poly.exponent3 != 0) counter++;
  if (poly.exponent5 != 0) counter++;
  if (poly.exponent7 != 0) counter++;
  return counter;
}

int SizeOptimizer::getInvocations(int maxPower, size_t num) {
  int count = 0;
  while (0 != num) {
    for (size_t p = maxPower; p > 0; p--) {
      if (num >= p) {
        num -= p;
        count++;
        break;
      }
    }
  }
  return count;
}

int SizeOptimizer::getInvocationsV8(Polynom &poly, bool isFloat) {
  int result = 0;
  if (isFloat) {
    result += getInvocations(V8_RADIX_2_MAX_SP, poly.exponent2);
    result += getInvocations(V8_RADIX_3_MAX_SP, poly.exponent3);
    result += getInvocations(V8_RADIX_5_MAX_SP, poly.exponent5);
    result += getInvocations(V8_RADIX_7_MAX_SP, poly.exponent7);
  } else {
    result += getInvocations(V8_RADIX_2_MAX_DP, poly.exponent2);
    result += getInvocations(V8_RADIX_3_MAX_DP, poly.exponent3);
    result += getInvocations(V8_RADIX_5_MAX_DP, poly.exponent5);
    result += getInvocations(V8_RADIX_7_MAX_DP, poly.exponent7);
  }
  return result;
}

int SizeOptimizer::getInvocations(Polynom &poly, bool isFloat) {
  switch (version) {
    case (CudaVersion::V_8):
      return getInvocationsV8(poly, isFloat);
    //	case (CudaVersion::V_9):
    //		return getInvocationsV9(poly); // FIXME implement
    default:
      throw std::domain_error("Unsupported version of CUDA");
  }
}

std::vector<SizeOptimizer::Polynom> *SizeOptimizer::generatePolys(
    size_t num, bool isFloat, bool crop) {
  std::vector<Polynom> *result = new std::vector<Polynom>();
  size_t maxPow2 = std::ceil(log(num) * log_2);
  size_t max = std::pow(2, maxPow2);
  size_t maxPow3 = std::ceil(std::log(max) * log_3);
  size_t maxPow5 = std::ceil(std::log(max) * log_5);
  size_t maxPow7 = std::ceil(std::log(max) * log_7);

  for (size_t a = 1; a <= maxPow2; a++) {  // we want at least one multiple of two
    for (size_t b = 0; b <= maxPow3; b++) {
      for (size_t c = 0; c <= maxPow5; c++) {
        for (size_t d = 0; d <= maxPow7; d++) {
          size_t value = std::pow(2, a) * std::pow(3, b)
            * std::pow(5, c) * std::pow(7, d);
          bool incCond = !crop && ((value >= num) && (value <= max));
          bool decrCond = crop && (value <= num);
          if (incCond || decrCond) {
            Polynom p;
            p.value = value;
            p.exponent2 = a;
            p.exponent3 = b;
            p.exponent5 = c;
            p.exponent7 = d;
            p.invocations = getInvocations(p, isFloat);
            p.noOfPrimes = getNoOfPrimes(p);
            result->push_back(p);
          }
        }
      }
    }
  }
  return result;
}

std::set<SizeOptimizer::Polynom, SizeOptimizer::valueComparator>
    *SizeOptimizer::filterOptimal(std::vector<SizeOptimizer::Polynom> *input,
    bool crop) {

  std::set<Polynom, valueComparator> *result =
    new std::set<Polynom, valueComparator>(!crop);

  size_t size = input->size();
  if (0 == size) {
    result->insert(UNIT);
    return result;
  }

  // add the most near polynom
  Polynom &minInv = input->at(0);
  Polynom &closest = minInv;
  for (size_t i = 1; i < size; i++) {
    Polynom &tmp = input->at(i);
    if (tmp.invocations < minInv.invocations) {
      // update min kernel invocations needed
      minInv = tmp;
    }
    if (closest.value > tmp.value) {
      closest = tmp;
    }
  }
  result->insert(closest);

  // add all polynoms which have a minimal number of kernel invocations
  for (size_t i = 0; i < size; i++) {
    Polynom &tmp = input->at(i);
    if ((tmp.invocations <= (minInv.invocations + 2)) &&
        (tmp.noOfPrimes <= 4)) {
      result->insert(tmp);
    }
  }
  return result;
}

}  // namespace cuFFTAdvisor
