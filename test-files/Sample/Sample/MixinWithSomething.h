//
//  ClassWithSomething.h
//  Sample
//
//

#import <Foundation/Foundation.h>

@interface MixinWithSomething : NSObject

- (instancetype)initWithName:(NSString*)name;

- (NSString*)invertName;

- (NSString*)concatenameWithPrefix:(NSString*)prefix
                             sufix:(NSString*)suffix;

@property (readonly) NSNumber* length;

@end

@interface MixinWithSomething (SomeWeirdExtension)

+ (BOOL) staticMemberForRescue:(MixinWithSomething*)parent;

@end
